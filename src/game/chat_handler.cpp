#include "game/chat_handler.hpp"
#include "game/chat_log.hpp"
#include "game/combat_log.hpp"
#include "core/config_paths.hpp"
#include "core/local_time.hpp"

#include <set>
#include "addons/lua_api_registrations.hpp"
#include "game/chat_filters.hpp"
#include "game/text_tokens.hpp"
#include "game/game_handler.hpp"
#include "game/game_utils.hpp"
#include "game/interface_text.hpp"
#include "game/zone_manager.hpp"
#include "game/packet_parsers.hpp"
#include "game/entity.hpp"
#include "game/opcode_table.hpp"
#include "game/update_field_table.hpp"
#include "network/world_socket.hpp"
#include "rendering/renderer.hpp"
#include "rendering/animation_controller.hpp"
#include "core/logger.hpp"
#include "core/application.hpp"
#include "addons/addon_manager.hpp"
#include "addons/lua_engine.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/dbc_layout.hpp"
#include "core/app_clock.hpp"
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <ctime>
#include "core/local_time.hpp"

namespace wowee {
namespace game {

ChatHandler::ChatHandler(GameHandler& owner)
    : owner_(owner) {
    initializeChatLog();
}

namespace {

bool isTruthyEnvValue(const std::string& value) {
    std::string lower;
    lower.reserve(value.size());
    for (unsigned char c : value) lower.push_back(static_cast<char>(std::tolower(c)));
    return lower == "1" || lower == "true" || lower == "yes" || lower == "on";
}

bool isFalsyEnvValue(const std::string& value) {
    std::string lower;
    lower.reserve(value.size());
    for (unsigned char c : value) lower.push_back(static_cast<char>(std::tolower(c)));
    return lower.empty() || lower == "0" || lower == "false" || lower == "no" || lower == "off";
}

std::string escapeChatLogField(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        switch (c) {
            case '\t': out += "\\t"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

std::string formatChatLogTimestamp(std::chrono::system_clock::time_point timestamp) {
    auto time = std::chrono::system_clock::to_time_t(timestamp);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        timestamp.time_since_epoch()) % 1000;

    std::tm tm = core::localTime(time);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S")
        << "." << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
}

std::string formatChatLogGuid(uint64_t guid) {
    if (guid == 0) return "";
    std::ostringstream oss;
    oss << "0x" << std::uppercase << std::hex
        << std::setfill('0') << std::setw(16) << guid;
    return oss.str();
}

std::string chatParticipant(const std::string& name, uint64_t guid, const char* fallback) {
    if (!name.empty()) return name;
    if (guid != 0) return formatChatLogGuid(guid);
    return fallback ? fallback : "";
}

/// A line an NPC or a boss said - the kinds whose text carries $-tokens.
bool isMonsterChatType(ChatType type) {
    switch (type) {
        case ChatType::MONSTER_SAY:
        case ChatType::MONSTER_PARTY:
        case ChatType::MONSTER_YELL:
        case ChatType::MONSTER_WHISPER:
        case ChatType::MONSTER_EMOTE:
        case ChatType::RAID_BOSS_EMOTE:
        case ChatType::RAID_BOSS_WHISPER:
            return true;
        default:
            return false;
    }
}

/// A player's guid: the type in its top sixteen bits is zero.
bool isPlayerGuid(uint64_t guid) {
    return guid != 0 && (guid >> 48) == 0;
}

/// The creature entry a creature's or vehicle's guid carries in bits 24-47,
/// or zero for any other kind of guid.
uint32_t creatureEntryOf(uint64_t guid) {
    const uint64_t high = guid >> 48;
    if (high != 0xF130 && high != 0xF150) return 0;
    return static_cast<uint32_t>((guid >> 24) & 0xFFFFFF);
}

/// The event a line goes to the interface as. A text emote with no sender name
/// is the one line chatframe.lua cannot print: it takes the gsub path only
/// when there is a name, and otherwise looks up CHAT_TEXT_EMOTE_GET, which no
/// GlobalStrings has - so it raised, and the line was lost. A monster emote is
/// printed from its text alone, which is all a nameless one has.
std::string chatEventNameFor(ChatType type, const std::string& shownName) {
    if (type == ChatType::TEXT_EMOTE && shownName.empty()) return "CHAT_MSG_MONSTER_EMOTE";
    return std::string("CHAT_MSG_") + getChatTypeString(type);
}

bool chatPacketDiagEnabled() {
    static const bool enabled = [] {
        const char* raw = std::getenv("WOWEE_CHAT_PACKET_DIAG");
        if (!raw) return false;
        return !isFalsyEnvValue(raw);
    }();
    return enabled;
}

} // namespace

void ChatHandler::initializeChatLog() {
    const char* enabledRaw = std::getenv("WOWEE_CHAT_LOG");
    if (!enabledRaw) return;

    std::string enabledValue(enabledRaw);
    if (isFalsyEnvValue(enabledValue)) return;

    const char* pathRaw = std::getenv("WOWEE_CHAT_LOG_PATH");
    const bool hasPathOverride = pathRaw && *pathRaw;
    chatLogPath_ = hasPathOverride ? pathRaw : "logs/chat.log";
    if (!hasPathOverride && !isTruthyEnvValue(enabledValue) && !enabledValue.empty()) {
        chatLogPath_ = enabledValue;
    }

    std::filesystem::path path(chatLogPath_);
    if (!path.parent_path().empty()) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
    }

    chatLogStream_.open(path, std::ios::out | std::ios::app);
    if (!chatLogStream_.is_open()) {
        LOG_WARNING("Chat external log requested but could not open: ", path.string());
        return;
    }

    chatLogEnabled_ = true;
    chatLogInitialized_ = true;
    chatLogStream_ << "# WoWee chat log started " << formatChatLogTimestamp(std::chrono::system_clock::now()) << '\n';
    chatLogStream_ << "# timestamp\tsource\ttype\tsender\tsender_guid\treceiver\treceiver_guid\tchannel\tmessage\n";
    chatLogStream_.flush();
    LOG_INFO("External chat logging enabled: ", path.string());
}

void ChatHandler::logChatMessage(const MessageChatData& msg, const char* source) {
    if (!chatLogEnabled_ || !chatLogInitialized_ || !chatLogStream_.is_open()) return;
    if (msg.message.empty()) return;

    const std::string sender = chatParticipant(msg.senderName, msg.senderGuid, "System");
    const std::string receiver = chatParticipant(msg.receiverName, msg.receiverGuid, "");
    chatLogStream_
        << formatChatLogTimestamp(msg.timestamp) << '\t'
        << (source ? source : "") << '\t'
        << getChatTypeString(msg.type) << '\t'
        << escapeChatLogField(sender) << '\t'
        << formatChatLogGuid(msg.senderGuid) << '\t'
        << escapeChatLogField(receiver) << '\t'
        << formatChatLogGuid(msg.receiverGuid) << '\t'
        << escapeChatLogField(msg.channelName) << '\t'
        << escapeChatLogField(msg.message) << '\n';
    chatLogStream_.flush();
}

bool ChatHandler::loggingChat(std::optional<bool> enable) {
    if (enable) {
        loggingChat_ = *enable;
        if (loggingChat_ && !chatLogFile_.is_open()) {
            // The same folder and opening as the combat log's (0x00461d10):
            // made if need be, the file added to.
            std::error_code ec;
            const std::filesystem::path dir = std::filesystem::path(core::getConfigRoot()) / "Logs";
            std::filesystem::create_directories(dir, ec);
            chatLogFile_.open(dir / "WoWChatLog.txt", std::ios::binary | std::ios::app);
            if (!chatLogFile_.is_open()) loggingChat_ = false;
        }
    }
    return loggingChat_;
}

void ChatHandler::closeChatLog() {
    if (chatLogFile_.is_open()) chatLogFile_.close();
    loggingChat_ = false;
}

void ChatHandler::writeChatLogLine(ChatType type, const std::string& text, const std::string& sender,
                                   const std::string& channelName, uint32_t language, const std::string& target) {
    if (!loggingChat_ || !chatLogFile_.is_open()) return;
    const uint32_t t = static_cast<uint32_t>(type);
    chat_log::Line l;
    l.type = t;
    l.text = text;
    l.sender = sender;
    l.target = target;
    if (!channelName.empty()) {
        const chat_channels::Slot* slot = channelSlots_.byName(channelName);
        l.channel = channelName;
        l.channelNumber = slot ? slot->number : 0;
    }
    // The language named where it is not Universal, not the player's own
    // (Orcish for the horde races, Common otherwise) and the type is not an
    // emote or a system line.
    static const std::set<uint8_t> kHordeRaces = {2, 5, 6, 8, 10};
    const uint32_t ownLanguage = kHordeRaces.count(owner_.getPlayerRace()) ? 1u : 7u;
    if (language != 0 && language != ownLanguage && t != 0x0a && t != 0x10 && t != 0x00 && t != 0x29)
        l.language = owner_.getLanguageName(language);
    const auto line = chat_log::line(l, [](const std::string& key, const std::vector<std::string>& args) {
        return interfaceFormat(key.c_str(), "", args);
    });
    if (!line) return;
    const auto now = std::chrono::system_clock::now();
    const std::tm tm = core::localTime(std::chrono::system_clock::to_time_t(now));
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    chatLogFile_ << combat_log::fileTimePrefix(static_cast<unsigned>(tm.tm_mon + 1),
                                               static_cast<unsigned>(tm.tm_mday),
                                               static_cast<unsigned>(tm.tm_hour),
                                               static_cast<unsigned>(tm.tm_min),
                                               static_cast<unsigned>(tm.tm_sec), static_cast<unsigned>(ms))
                 << *line << "\r\n";
}

void ChatHandler::handleExpectedSpamRecords(network::Packet& packet) {
    // SMSG_EXPECTED_SPAM_RECORDS (0x00501c70): a count, then that many
    // null-terminated regular expressions the server wants matched against chat.
    if (!packet.hasRemaining(4)) return;
    const uint32_t count = packet.readUInt32();
    std::vector<std::string> patterns;
    patterns.reserve(count);
    for (uint32_t i = 0; i < count && packet.hasRemaining(1); ++i) {
        patterns.push_back(packet.readString());
    }
    spamFilter_.setServerPatterns(patterns);
    LOG_DEBUG("SMSG_EXPECTED_SPAM_RECORDS: ", spamFilter_.serverPatternCount(), " of ", count,
              " spam patterns compiled");
}

bool ChatHandler::dropAsSpam(const MessageChatData& data) {
    // 0x00509dd0: not a system line nor one the server already filtered (0x2b),
    // not from a GM or a developer by the chat tag ("GM" for 0x04, "DEV" for
    // 0x10, 0x0050be70), nor from a sender carrying PLAYER_FLAGS_GM (0x8), and
    // only while spamFilter is on.
    if (data.type == ChatType::SYSTEM || data.type == ChatType::FILTERED) return false;
    if ((data.chatTag & 0x14u) != 0) return false;
    if (addons::storedCVarValue("spamFilter", "1") == "0") return false;
    if (data.senderGuid != 0) {
        const auto sender = owner_.getEntityManager().getEntity(data.senderGuid);
        const uint16_t flagsIdx = fieldIndex(UF::PLAYER_FLAGS);
        if (sender && sender->getType() == ObjectType::PLAYER && flagsIdx != 0xFFFF &&
            (sender->getField(flagsIdx) & 0x8u) != 0) {
            return false;
        }
    }
    // SpamMessages.dbc's own list, read the first time a line is asked about
    // (0x007e2250 reads it with the other chat filters).
    if (!spamMessagesLoaded_) {
        auto* am = owner_.services().assetManager;
        const auto* layouts = pipeline::getActiveDBCLayout();
        if (am && am->isInitialized() && layouts) {
            spamMessagesLoaded_ = true;
            std::vector<std::string> patterns;
            if (const auto* layout = layouts->getLayout("SpamMessages")) {
                const uint32_t textF = layout->tryField("Text");
                auto dbc = am->loadDBCOptional("SpamMessages.dbc");
                if (dbc && dbc->isLoaded() && textF != 0xFFFFFFFFu && textF < dbc->getFieldCount()) {
                    for (uint32_t r = 0; r < dbc->getRecordCount(); ++r) patterns.push_back(dbc->getString(r, textF));
                }
            }
            spamFilter_.setClientPatterns(patterns);
            LOG_INFO("Chat spam filter: ", spamFilter_.clientPatternCount(), " SpamMessages.dbc patterns");
        }
    }
    if (!spamFilter_.matches(data.message)) return false;
    // A spam whisper is reported to the server by its sender (0x331), then
    // dropped like the rest.
    if (data.type == ChatType::WHISPER && data.senderGuid != 0 && owner_.getSocket() &&
        wireOpcode(Opcode::CMSG_CHAT_FILTERED) != 0xFFFF) {
        network::Packet pkt(wireOpcode(Opcode::CMSG_CHAT_FILTERED));
        pkt.writeUInt64(data.senderGuid);
        owner_.getSocket()->send(pkt);
    }
    LOG_DEBUG("Chat line dropped by the spam filter");
    return true;
}

void ChatHandler::registerOpcodes(DispatchTable& table) {
    table[Opcode::SMSG_MESSAGECHAT] = [this](network::Packet& packet) {
        if (owner_.getState() == WorldState::IN_WORLD) handleMessageChat(packet);
    };
    table[Opcode::SMSG_GM_MESSAGECHAT] = [this](network::Packet& packet) {
        if (owner_.getState() != WorldState::IN_WORLD) return;
        // SMSG_GM_MESSAGECHAT is SMSG_MESSAGECHAT read with the GM flag
        // (0x0050ebc0 -> 0x0050be70(.., 1)): the sender's name comes after the
        // header for the types read through the reader's general branch - not
        // the creature-spoken types, the foreign whisper, the battleground
        // announcements or the achievements - and the client keeps it as the
        // line's sender rather than dropping it.
        if (!packet.hasRemaining(17)) return;
        uint8_t  type       = packet.readUInt8();
        uint32_t lang       = packet.readUInt32();
        uint64_t senderGuid = packet.readUInt64();
        uint32_t unk        = packet.readUInt32();
        std::string gmName;
        if (gmChatCarriesSenderName(type) && !packet.readSizedString(gmName)) return;

        // Rebuild as regular SMSG_MESSAGECHAT (header + remaining body)
        network::Packet regular(0);
        regular.writeUInt8(type);
        regular.writeUInt32(lang);
        regular.writeUInt64(senderGuid);
        regular.writeUInt32(unk);
        const auto body = packet.remainingData();
        if (!body.empty())
            regular.writeBytes(body.data(), body.size());
        handleMessageChat(regular, gmName);
    };
    table[Opcode::SMSG_TEXT_EMOTE] = [this](network::Packet& packet) {
        if (owner_.getState() == WorldState::IN_WORLD) handleTextEmote(packet);
    };
    // The server's spam regexes, which mask gold-seller lines in player chat.
    table[Opcode::SMSG_EXPECTED_SPAM_RECORDS] = [this](network::Packet& packet) {
        handleExpectedSpamRecords(packet);
    };
    table[Opcode::SMSG_EMOTE] = [this](network::Packet& packet) {
        if (owner_.getState() != WorldState::IN_WORLD) return;
        if (!packet.hasRemaining(12)) return;
        uint32_t emoteId    = packet.readUInt32();
        uint64_t sourceGuid = packet.readUInt64();
        uint32_t animId = rendering::AnimationController::getEmoteAnimByEmotesId(emoteId);
        // Emotes.dbc EmoteSpecProc distinguishes persistent STATE_ emotes from
        // one-shots. Emote 0 (ONESHOT_NONE) cancels a one-shot but must not
        // clear a UNIT_NPC_EMOTESTATE work loop, so it is forwarded as non-state.
        const bool isState = emoteId != 0 &&
            rendering::AnimationController::isStateEmoteById(emoteId);
        if (owner_.emoteAnimCallbackRef() && sourceGuid != 0 && (animId != 0 || emoteId == 0)) {
            owner_.emoteAnimCallbackRef()(sourceGuid, animId, isState);
        } else if (emoteId != 0 && animId == 0) {
            LOG_DEBUG("SMSG_EMOTE emoteId=", emoteId, " had no Emotes.dbc animation mapping");
        }
    };
    table[Opcode::SMSG_CHANNEL_NOTIFY] = [this](network::Packet& packet) {
        if (owner_.getState() == WorldState::IN_WORLD ||
            owner_.getState() == WorldState::ENTERING_WORLD)
            handleChannelNotify(packet);
    };
    // The chat refusals (0x006e2e90). Each is a game error of the client's
    // table at 0x00ac8af0, which 0x005216f0 prints as a CHAT_MSG_SYSTEM line -
    // not the red on-screen error these were raised as, twice over, in
    // English.
    table[Opcode::SMSG_CHAT_PLAYER_NOT_FOUND] = [this](network::Packet& packet) {
        // ERR_CHAT_PLAYER_NOT_FOUND_S (error 0x100) with the name.
        std::string name = packet.readString();
        addSystemChatMessage(interfaceFormat("ERR_CHAT_PLAYER_NOT_FOUND_S",
                                             "No player named '%s' is currently playing.", {name}));
    };
    table[Opcode::SMSG_CHAT_PLAYER_AMBIGUOUS] = [this](network::Packet& packet) {
        // ERR_CHAT_PLAYER_AMBIGUOUS_S (error 0x1fa) with the name.
        std::string name = packet.readString();
        addSystemChatMessage(interfaceFormat("ERR_CHAT_PLAYER_AMBIGUOUS_S",
            "%s: More than one player has that name. Add the realm name after it.", {name}));
    };
    table[Opcode::SMSG_CHAT_WRONG_FACTION] = [this](network::Packet& /*packet*/) {
        // ERR_CHAT_WRONG_FACTION (error 0xff).
        addSystemChatMessage(interfaceString("ERR_CHAT_WRONG_FACTION",
                                             "You can only whisper to members of your faction."));
    };
    table[Opcode::SMSG_CHAT_NOT_IN_PARTY] = [this](network::Packet& packet) {
        // The chat type refused: party or party leader is ERR_NOT_IN_GROUP
        // (0x50); raid, raid leader or raid warning ERR_NOT_IN_RAID (0x1bd);
        // any other says nothing.
        if (!packet.hasRemaining(4)) return;
        switch (static_cast<ChatType>(packet.readUInt32())) {
            case ChatType::PARTY:
            case ChatType::PARTY_LEADER:
                addSystemChatMessage(interfaceString("ERR_NOT_IN_GROUP", "You aren't in a party."));
                break;
            case ChatType::RAID:
            case ChatType::RAID_LEADER:
            case ChatType::RAID_WARNING:
                addSystemChatMessage(interfaceString("ERR_NOT_IN_RAID", "You are not in a raid group."));
                break;
            default: break;
        }
    };
    table[Opcode::SMSG_CHAT_RESTRICTED] = [this](network::Packet& packet) {
        // A byte says which restriction: errors 0x1ef, 0x1f0, 0x231, 0x2a7.
        if (!packet.hasRemaining(1)) return;
        switch (packet.readUInt8()) {
            case 0:
                addSystemChatMessage(interfaceString("ERR_CHAT_RESTRICTED",
                    "Trial accounts may only whisper a limited number of players. "
                    "You must wait before whispering someone new."));
                break;
            case 1:
                addSystemChatMessage(interfaceString("ERR_CHAT_THROTTLED",
                    "The number of messages that can be sent is limited. Please wait to send another message."));
                break;
            case 2:
                addSystemChatMessage(interfaceString("ERR_USER_SQUELCHED",
                    "You cannot send messages or mail while pending a Game Master review."));
                break;
            case 3:
                addSystemChatMessage(interfaceString("ERR_YELL_RESTRICTED", "Trial accounts cannot yell."));
                break;
            default: break;
        }
    };

    // ---- Channel list ----

    // ---- Server / defense / area-trigger messages (moved from GameHandler) ----
    table[Opcode::SMSG_DEFENSE_MESSAGE] = [this](network::Packet& packet) {
        // The area, then the text with its length in front (0x0050c850).
        // This read the text from the length's bytes and printed it as a
        // system line after "[Defense] ". The client prints it as a line of
        // each joined defense channel (ChatChannels.dbc flag 0x10000) - of a
        // zone-dependent one only when the area's zone is the player's.
        if (!packet.hasRemaining(8)) return;
        uint32_t areaId = packet.readUInt32();
        /*uint32_t length =*/ packet.readUInt32();
        const std::string text = packet.readString();
        auto* renderer = core::Application::getInstance().getRenderer();
        // The area's parent where it has one (AreaTable +8), compared with
        // the zone the player is in (0x00bd080c).
        if (auto* zm = renderer ? renderer->getZoneManager() : nullptr) {
            if (const auto parent = zm->areaParent(areaId); parent && *parent != 0) areaId = *parent;
        }
        const auto* place = renderer ? renderer->getZoneTexts() : nullptr;
        const uint32_t playerZone = place ? place->zoneId : 0;
        for (const auto& slot : channelSlots_.all()) {
            if (slot.number == 0 || slot.pending || slot.zoneChannelId == 0) continue;
            const chat_channels::Row* row = nullptr;
            for (const auto& r : channelRows()) if (r.id == slot.zoneChannelId) { row = &r; break; }
            if (!row || (row->flags & chat_channels::kFlagDefense) == 0) continue;
            if ((row->flags & chat_channels::kFlagZoneDependent) != 0 && areaId != playerZone) continue;
            // From no one: the line is the channel's, as 0x00509dd0 is
            // handed it with no sender.
            MessageChatData line;
            line.type = ChatType::CHANNEL;
            line.language = ChatLanguage::UNIVERSAL;
            line.message = text;
            line.channelName = slot.name;
            addLocalChatMessage(line);
        }
    };
    // ServerMessages.dbc's text for the type with the server's text put in,
    // a CHAT_MSG_SYSTEM line (0x0050c980): "[SERVER] Shutdown in %s" and the
    // rest, as the realm's locale words them. A type the file does not have
    // is "[%d]: %s". This put English of its own in front - "[Shutdown] ",
    // "[Announcement] " - and raised shutdown and restart as red errors too.
    // SMSG_SERVER_MESSAGE is the older expansions' name for the same 0x291.
    auto serverMessage = [this](network::Packet& packet) {
        if (!packet.hasRemaining(4)) return;
        const uint32_t type = packet.readUInt32();
        const std::string text = packet.readString();
        std::string format;
        auto* am = owner_.services().assetManager;
        if (am && am->isInitialized()) {
            if (auto dbc = am->loadDBC("ServerMessages.dbc"); dbc && dbc->isLoaded()) {
                // ID, then the text's locale columns (and a flags column).
                const uint32_t fields = dbc->getFieldCount();
                const uint32_t width = fields >= 18 ? 16u : fields >= 10 ? 8u : 1u;
                for (uint32_t i = 0; i < dbc->getRecordCount() && fields >= 2; ++i) {
                    if (dbc->getUInt32(i, 0) != type) continue;
                    format = chat_channels::localizedText(
                        [&](uint32_t f) { return f < fields ? dbc->getString(i, f) : std::string(); },
                        1, std::min(width, fields - 1));
                    break;
                }
            }
        }
        std::string line;
        if (format.empty()) line = "[" + std::to_string(type) + "]: " + text;
        else if (text.empty()) line = format;
        else line = chat_channels::luaFormat(format, {text});
        addSystemChatMessage(line);
    };
    table[Opcode::SMSG_SERVER_MESSAGE] = serverMessage;
    table[Opcode::SMSG_CHAT_SERVER_MESSAGE] = serverMessage;
    table[Opcode::SMSG_AREA_TRIGGER_MESSAGE] = [this](network::Packet& packet) {
        // UI_INFO_MESSAGE with the server's text, and nothing more
        // (0x00526530, case 0x2b8: event 0xbc). This raised it as a red error,
        // wrote it to the chat log, and queued a toast of it as well.
        if (packet.hasRemaining(4)) {
            /*uint32_t len =*/ packet.readUInt32();
            std::string msg = packet.readString();
            if (!msg.empty()) owner_.addUIInfoMessage(msg);
        }
    };

    table[Opcode::SMSG_CHANNEL_LIST] = [this](network::Packet& p) { handleChannelList(p); };
    // Add (0x3f0 -> 0x00504130) and update (0x3f2 -> 0x00500380) are separate
    // routines in the client and answer differently - the one appends, the
    // other only changes a row it already has - but they read the same fields,
    // so one reader with the kind carried through is as far as they merge.
    table[Opcode::SMSG_USERLIST_ADD] = [this](network::Packet& p) { handleUserlistAdd(p, UserlistChange::add); };
    table[Opcode::SMSG_USERLIST_UPDATE] = [this](network::Packet& p) { handleUserlistAdd(p, UserlistChange::update); };
    table[Opcode::SMSG_USERLIST_REMOVE] = [this](network::Packet& p) { handleUserlistRemove(p); };
}

void ChatHandler::sendChatMessage(ChatType type, const std::string& message, const std::string& target) {
    if (owner_.getState() != WorldState::IN_WORLD) {
        LOG_WARNING("Cannot send chat in state: ", static_cast<int>(owner_.getState()));
        return;
    }

    // Away and busy are chat types of their own, which the client words and
    // sends itself (0x0050d170) - SendChatMessage(msg, "AFK") is /afk. Sent
    // as any other line, this cleared the away flag before setting it, sent
    // nothing for an empty message and echoed the line into the chat.
    if (type == ChatType::AFK) { toggleAfk(message); return; }
    if (type == ChatType::DND) { toggleDnd(message); return; }

    // Auto Clear AFK, which the panel offers and nothing read: the flag was
    // only ever cleared by typing /afk a second time, so a player who came
    // back and started talking stayed away as far as everyone else could see,
    // still auto-replying to whispers.
    //
    // Sending a message is the signal used here. It is not the only one WoW
    // takes - moving clears it too - but it is the one that matters, because
    // it is the one where the player is visibly present and being answered
    // for by a machine.
    if (owner_.afkStatusRef() &&
        addons::storedCVarValue("autoClearAFK", "1") != "0") {
        toggleAfk("");
    }

    if (message.empty()) {
        LOG_WARNING("Cannot send empty chat message");
        return;
    }

    LOG_INFO("OUTGOING CHAT: type=", static_cast<int>(type),
             " (", getChatTypeString(type), ") target='", target, "' msg='", message.substr(0, 60), "'");

    // Use the player's faction language. AzerothCore rejects wrong language.
    // Alliance races: Human(1), Dwarf(3), NightElf(4), Gnome(7), Draenei(11) → COMMON (7)
    // Horde races: Orc(2), Undead(5), Tauren(6), Troll(8), BloodElf(10) → ORCISH (1)
    uint8_t race = owner_.getPlayerRace();
    bool isHorde = (race == 2 || race == 5 || race == 6 || race == 8 || race == 10);
    ChatLanguage language = isHorde ? ChatLanguage::ORCISH : ChatLanguage::COMMON;

    auto packet = MessageChatPacket::build(type, language, message, target);
    if (chatPacketDiagEnabled()) {
        const auto& raw = packet.getData();
        LOG_WARNING("CHAT PACKET DIAG TX CMSG_MESSAGECHAT type=", getChatTypeString(type),
                    " target='", target, "' bytes=", raw.size(), " data=[",
                    raw.empty() ? std::string{} : core::toHexString(raw.data(), raw.size(), true), "]");
    }
    owner_.getSocket()->send(packet);

    // Add local echo so the player sees their own message immediately
    MessageChatData echo;
    echo.senderGuid = owner_.getPlayerGuid();
    echo.language = language;
    echo.message = message;

    auto nameIt = owner_.getPlayerNameCache().find(owner_.getPlayerGuid());
    if (nameIt != owner_.getPlayerNameCache().end()) {
        echo.senderName = nameIt->second;
    } else if (const Character* active = owner_.getActiveCharacter()) {
        echo.senderName = active->name;
    }

    if (type == ChatType::WHISPER) {
        echo.type = ChatType::WHISPER_INFORM;
        echo.receiverName = target;
    } else {
        echo.type = type;
    }

    if (type == ChatType::CHANNEL) {
        echo.channelName = target;
    }

    addLocalChatMessage(echo);
}

void ChatHandler::sendAddonMessage(ChatType type, const std::string& message, const std::string& target) {
    if (owner_.getState() != WorldState::IN_WORLD || message.empty()) return;

    auto packet = MessageChatPacket::build(type, ChatLanguage::ADDON, message, target);
    if (chatPacketDiagEnabled()) {
        const auto& raw = packet.getData();
        LOG_WARNING("CHAT PACKET DIAG TX CMSG_MESSAGECHAT addon type=", getChatTypeString(type),
                    " target='", target, "' bytes=", raw.size(), " data=[",
                    raw.empty() ? std::string{} : core::toHexString(raw.data(), raw.size(), true), "]");
    }
    owner_.getSocket()->send(packet);
}

void ChatHandler::handleMessageChat(network::Packet& packet, const std::string& gmName) {
    LOG_DEBUG("Handling SMSG_MESSAGECHAT");
    if (chatPacketDiagEnabled()) {
        const auto& raw = packet.getData();
        LOG_WARNING("CHAT PACKET DIAG RX SMSG_MESSAGECHAT bytes=", raw.size(), " data=[",
                    raw.empty() ? std::string{} : core::toHexString(raw.data(), raw.size(), true), "]");
    }

    MessageChatData data;
    if (!owner_.getPacketParsers()->parseMessageChat(packet, data)) {
        const auto& raw = packet.getData();
        LOG_WARNING("Failed to parse SMSG_MESSAGECHAT, size=", packet.getSize(),
                    " data=[", raw.empty() ? std::string{} : core::toHexString(raw.data(), raw.size(), true), "]");
        return;
    }
    if (chatPacketDiagEnabled()) {
        LOG_WARNING("CHAT PACKET DIAG PARSED SMSG_MESSAGECHAT type=", getChatTypeString(data.type),
                    " sender=", formatChatLogGuid(data.senderGuid), " senderName='", data.senderName,
                    "' receiver=", formatChatLogGuid(data.receiverGuid), " receiverName='", data.receiverName,
                    "' channel='", data.channelName, "' msg='", data.message, "'");
    }
    if (!gmName.empty()) data.senderName = gmName;
    LOG_DEBUG("INCOMING CHAT: type=", static_cast<int>(data.type),
             " (", getChatTypeString(data.type), ") sender=0x", std::hex, data.senderGuid, std::dec,
             " '", data.senderName, "' msg='", data.message.substr(0, 60), "'");

    // WoW servers echo successful outgoing whispers as WHISPER_INFORM, but the
    // client already creates a local WHISPER_INFORM row immediately on send.
    if (data.type == ChatType::WHISPER_INFORM) {
        return;
    }

    // Skip server echo of our own messages (we already added a local echo)
    if (data.senderGuid == owner_.getPlayerGuid() && data.senderGuid != 0) {
        if (data.type == ChatType::WHISPER && !data.senderName.empty()) {
            owner_.lastWhisperSenderRef() = data.senderName;
        }
        return;
    }

    deliverChatMessage(std::move(data), /*alreadyWaited=*/false);
}

void ChatHandler::deliverChatMessage(MessageChatData data, bool alreadyWaited) {
    // Resolve sender name from entity/cache if not already set by parser
    if (data.senderName.empty() && data.senderGuid != 0) {
        auto nameIt = owner_.getPlayerNameCache().find(data.senderGuid);
        if (nameIt != owner_.getPlayerNameCache().end()) {
            data.senderName = nameIt->second;
        } else {
            auto entity = owner_.getEntityManager().getEntity(data.senderGuid);
            if (entity) {
                if (entity->getType() == ObjectType::PLAYER) {
                    auto player = std::dynamic_pointer_cast<Player>(entity);
                    if (player && !player->getName().empty()) {
                        data.senderName = player->getName();
                    }
                } else if (entity->getType() == ObjectType::UNIT) {
                    auto unit = std::dynamic_pointer_cast<Unit>(entity);
                    if (unit && !unit->getName().empty()) {
                        data.senderName = unit->getName();
                    }
                }
            }
        }

        if (data.senderName.empty()) {
            const auto& partyData = owner_.getPartyData();
            for (const auto& member : partyData.members) {
                if (member.guid == data.senderGuid && !member.name.empty()) {
                    data.senderName = member.name;
                    break;
                }
            }
        }

        // The guild roster, which was not among the places looked and is the
        // one that holds the names this most often needs.
        //
        // Every other source here is someone nearby or in the party. A
        // guildmate is characteristically neither: they are in another zone,
        // which is the whole point of guild chat. So a guild line from anyone
        // not recently met arrived with no name at all - "[Guild] : message" -
        // and a guild achievement, which is announced by whoever earned it
        // wherever they are, read "[] has earned the achievement".
        //
        // The roster is already held and already carries the name beside the
        // guid; nothing had asked it. It is only populated once the roster has
        // been requested, so this is a source rather than a guarantee, and the
        // query below still runs when it misses.
        if (data.senderName.empty()) {
            for (const auto& member : owner_.getGuildRoster().members) {
                if (member.guid == data.senderGuid && !member.name.empty()) {
                    data.senderName = member.name;
                    break;
                }
            }
        }

        // A creature's name, from what the creature queries have brought back,
        // when it is not in sight to be read off. A player name query about
        // one is never answered, so the line waited out the whole delay and
        // then went without a name.
        if (data.senderName.empty() && !isPlayerGuid(data.senderGuid)) {
            if (const uint32_t entry = creatureEntryOf(data.senderGuid)) {
                data.senderName = owner_.getCachedCreatureName(entry);
                if (data.senderName.empty()) owner_.queryCreatureInfo(entry, data.senderGuid);
            }
        }

        if (data.senderName.empty() && isPlayerGuid(data.senderGuid)) {
            owner_.queryPlayerName(data.senderGuid);
            // Hold the line rather than showing it with nobody's name on it.
            //
            // SMSG_MESSAGECHAT carries a guid and no name for a player line,
            // and every source above is somebody nearby, in the party or in
            // the guild. A whisper is characteristically from none of those -
            // that is what makes it a whisper - so it missed all of them, went
            // out with an empty name, and the interface printed it with no
            // sender. The backfill that follows a name query reaches this
            // client's own history and not what the interface has already
            // drawn, so the name has to be there the first time.
            if (!alreadyWaited) {
                const uint64_t waitingOn = data.senderGuid;
                chatAwaitingName_.push_back({std::move(data), waitingOn,
                                             core::appTimeSeconds() + kNameWaitSeconds});
                return;
            }
        }
    }

    // And the player an NPC's line is written for, when that is someone else.
    //
    // "$N has defeated the hero of the Warmaul" is a yell the whole area hears,
    // and $N is whoever earned it: the packet names them as the receiver. This
    // wrote the logged-in character's name into every such line, so each
    // bystander read that they had done it themselves. A player receiver comes
    // as a guid alone, so a name nothing holds yet is asked for and the line
    // waits for it, as a player's own line does.
    if (isMonsterChatType(data.type) && data.message.find('$') != std::string::npos &&
        isPlayerGuid(data.receiverGuid) && data.receiverGuid != owner_.getPlayerGuid() &&
        knownPlayerName(data.receiverGuid).empty()) {
        owner_.queryPlayerName(data.receiverGuid);
        if (!alreadyWaited) {
            const uint64_t waitingOn = data.receiverGuid;
            chatAwaitingName_.push_back({std::move(data), waitingOn,
                                         core::appTimeSeconds() + kNameWaitSeconds});
            return;
        }
    }

    if (data.message.empty()) {
        return;
    }

    if (data.type == ChatType::ACHIEVEMENT ||
        data.type == ChatType::GUILD_ACHIEVEMENT) {
        auto replaceAll = [](std::string& text, const std::string& marker,
                             const std::string& replacement) {
            size_t pos = 0;
            while ((pos = text.find(marker, pos)) != std::string::npos) {
                text.replace(pos, marker.size(), replacement);
                pos += replacement.size();
            }
        };

        if (data.achievementId != 0) {
            owner_.ensureAchievementNamesLoaded();
            std::string achievementName = owner_.getAchievementName(data.achievementId);
            if (achievementName.empty()) {
                achievementName = "Achievement #" + std::to_string(data.achievementId);
            }
            std::ostringstream link;
            link << "|cffffff00|Hachievement:" << data.achievementId
                 << ":0:0:0:0:0:0:0:0:0|h[" << achievementName << "]|h|r";
            replaceAll(data.message, "$a", link.str());
        }
    }

    // Addon messages use the same chat types as player messages, but belong to
    // CHAT_MSG_ADDON and must never be inserted into visible chat history.
    std::string addonPrefix;
    std::string addonPayload;
    if (decodeAddonChatPayload(data, addonPrefix, addonPayload)) {
        if (owner_.addonEventCallbackRef() && !addonPrefix.empty()) {
            owner_.addonEventCallbackRef()(
                "CHAT_MSG_ADDON",
                {addonPrefix, addonPayload, getChatTypeString(data.type), data.senderName});
        }
        return;
    }

    // Server monster messages use %s as a placeholder for the creature's name.
    if (!data.senderName.empty() && (
            data.type == ChatType::MONSTER_SAY || data.type == ChatType::MONSTER_YELL ||
            data.type == ChatType::MONSTER_EMOTE || data.type == ChatType::MONSTER_WHISPER ||
            data.type == ChatType::MONSTER_PARTY ||
            data.type == ChatType::RAID_BOSS_EMOTE || data.type == ChatType::RAID_BOSS_WHISPER)) {
        size_t pos = data.message.find("%s");
        while (pos != std::string::npos) {
            data.message.replace(pos, 2, data.senderName);
            pos = data.message.find("%s", pos + data.senderName.size());
        }
    }

    // Filter BG/Arena queue announcer spam (server-side modules on
    // ChromieCraft/AzerothCore). Common formats:
    //   |cffff0000[BG Queue Announcer]:|r ...
    //   |cffff0000[Arena Queue Announcer]:|r ...
    //   |cFFFFA500<player> joined : |cFF00FFFF2x2|r
    //   |cFFFFA500<player> exited |cFF00FFFF3x3|r
    // The third/fourth forms drop the "Queue Announcer" prefix entirely, so
    // we also detect the announcer-shaped pattern: a colored message that
    // names an arena/BG bracket suffix like "2x2|r" / "3v3|r".
    {
        const auto& msg = data.message;
        auto containsCI = [&](const char* needle) {
            const size_t nlen = std::strlen(needle);
            if (msg.size() < nlen) return false;
            const size_t last = msg.size() - nlen;
            for (size_t i = 0; i <= last; ++i) {
                bool match = true;
                for (size_t j = 0; j < nlen; ++j) {
                    unsigned char a = static_cast<unsigned char>(msg[i + j]);
                    unsigned char b = static_cast<unsigned char>(needle[j]);
                    if (std::tolower(a) != std::tolower(b)) { match = false; break; }
                }
                if (match) return true;
            }
            return false;
        };
        if (containsCI("queue announcer") || containsCI("queue status")) {
            return;
        }
        // Pattern-based catch for prefix-less variants. Require the message to
        // contain a color code (server-formatted) AND an arena/BG bracket token
        // immediately followed by |r AND a verb word ("joined", "exited",
        // "left", "entered"). Plain player chat won't hit all three.
        const bool hasColor = msg.find("|c") != std::string::npos;
        if (hasColor) {
            static const char* kBracketTokens[] = {
                "2x2|r", "3x3|r", "5x5|r",
                "2v2|r", "3v3|r", "5v5|r",
                "2X2|r", "3X3|r", "5X5|r",
                "2V2|r", "3V3|r", "5V5|r",
            };
            bool hasBracket = false;
            for (const char* t : kBracketTokens) {
                if (msg.find(t) != std::string::npos) { hasBracket = true; break; }
            }
            if (hasBracket && (containsCI("joined") || containsCI("exited") ||
                               containsCI(" left ") || containsCI("entered") ||
                               containsCI("queue"))) {
                return;
            }
        }
    }

    // Filter officer chat if player doesn't have officer chat permission.
    // Some servers send officer chat to all guild members regardless of rank.
    // WoW guild right bit 0x40 = GR_RIGHT_OFFCHATSPEAK, 0x80 = GR_RIGHT_OFFCHATLISTEN
    if (data.type == ChatType::OFFICER) {
        // Through the shared lookup, which answers zero rights when the roster
        // has not arrived - the same "say nothing about it" this had before,
        // since the test below only hides chat when a rank is known and lacks
        // the bit.
        const uint32_t idx = owner_.getPlayerGuildRankIndex();
        if (idx != 0xFFFFFFFFu && idx < owner_.getGuildRoster().ranks.size()) {
            if (!(owner_.getPlayerGuildRankRights() & 0x80)) { // GR_RIGHT_OFFCHATLISTEN
                return; // Don't show officer chat to non-officers
            }
        }
    }

    // Filter addon-to-addon whispers (GearScore, DBM, oRA, etc.) from player chat.
    // These are invisible in the real WoW client.
    if (data.type == ChatType::WHISPER || data.type == ChatType::WHISPER_INFORM) {
        const auto& msg = data.message;
        if (msg.size() >= 3 && (
            msg.rfind("GS_", 0) == 0 ||          // GearScore
            msg.rfind("DVNE", 0) == 0 ||          // DBM (DeadlyBossMods)
            msg.rfind("oRA", 0) == 0 ||            // oRA raid addon
            msg.rfind("BWVQ", 0) == 0 ||           // BigWigs
            msg.rfind("AVR", 0) == 0 ||            // AVR (Augmented Virtual Reality)
            msg.rfind('\t', 0) == 0 ||             // Tab-prefixed addon messages
            (msg.size() > 4 && static_cast<unsigned char>(msg[0]) > 127))) {  // Binary data
            return; // Silently discard addon whisper
        }
    }

    // Fill in the $-tokens before anything sees the line.
    //
    // A monster's say, yell, emote or whisper arrives with the player left as a
    // blank - "$N, you have done well" - and the client is what writes the name
    // in. Nothing did, so every scripted NPC in the game addressed the player as
    // "$N", and a $gsir:madam; came out with the whole switch printed.
    //
    // Only what an NPC or a boss said. A player typing "$N" into say has typed
    // those two characters and means them: the real client does not resolve
    // tokens in player chat, and resolving them here would let one player make
    // another's client print that player's own name.
    //
    // For whoever the line is written for, which is not always the reader:
    // see monsterLineSubject.
    if (isMonsterChatType(data.type)) {
        data.message = resolveTextTokens(data.message, monsterLineSubject(data));
    }

    // Add to chat history
    data.uid = ++chatUidCounter_;
    chatHistory_.push_back(data);
    if (chatHistory_.size() > maxChatHistory_) {
        chatHistory_.erase(chatHistory_.begin());
    }
    logChatMessage(data, "server");
    // No fireChatEvent here. This function announces the message itself,
    // further down and with the fuller argument list - the channel number and
    // short name, the line id, the sender's guid. Calling fireChatEvent as
    // well fired CHAT_MSG_* twice for every line the server sent, and the
    // interface drew both: every whisper, say, guild and channel message
    // appeared in the window doubled.
    //
    // The note that stood here said the event was fired only on the local
    // path, which was true when it was written and stopped being true when
    // this function grew its own. fireChatEvent stays for
    // addLocalChatMessage's callers, which have no richer arguments to give.

    // Track whisper sender for /r command
    if (data.type == ChatType::WHISPER) {
        // Always store GUID so getLastWhisperSender() can resolve the name
        // from the player name cache even if name wasn't available yet
        if (data.senderGuid != 0)
            owner_.lastWhisperSenderGuidRef() = data.senderGuid;
        if (!data.senderName.empty())
            owner_.lastWhisperSenderRef() = data.senderName;
        // No answer from here. The away and busy replies are the server's,
        // to the player who whispered, now that toggleAfk and toggleDnd tell
        // it; this whispered "<AFK> Away from Keyboard" back itself, in
        // English, which the real client never does.
    }

    // The Social panel's two filters, before anything shows this line: the
    // bubble below, the CHAT_MSG_ event, and the log all take the text from
    // here, so filtering at one point covers all three.
    //
    // Only what a player typed. A quest giver, a boss emote or a system notice
    // is not somebody spamming, and masking words in them would edit the
    // game's own writing.
    // The spam filter (0x00509dd0 -> 0x00509bd0): a line matching
    // SpamMessages.dbc's or the server's patterns goes no further - no bubble,
    // no event, no log. After the whisper's reply target is kept, as the client
    // keeps it (0x0050be70) before the line is shown.
    if (dropAsSpam(data)) return;

    const bool fromAPlayer =
        data.type == ChatType::SAY || data.type == ChatType::YELL ||
        data.type == ChatType::PARTY || data.type == ChatType::RAID ||
        data.type == ChatType::GUILD || data.type == ChatType::OFFICER ||
        data.type == ChatType::WHISPER || data.type == ChatType::EMOTE ||
        data.type == ChatType::CHANNEL;

    if (fromAPlayer) {
        // Disable Spam Filter, so the CVar being on means filtering happens.
        if (addons::storedCVarValue("spamFilter", "1") != "0") {
            const double now = std::chrono::duration<double>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            if (repeatsRecentLine(recentChatLines_, data.senderGuid, data.message, now)) {
                return;
            }
            recentChatLines_.push_back({.senderGuid = data.senderGuid, .text = data.message, .at = now});
            // A short memory is the point: this is looking for a line pasted
            // again a moment later, not keeping a record of the conversation.
            while (recentChatLines_.size() > 64) recentChatLines_.pop_front();
        }
    }

    // Trigger chat bubble for SAY/YELL messages from others
    if (owner_.chatBubbleCallbackRef() && data.senderGuid != 0) {
        bool bubble = (data.type == ChatType::SAY || data.type == ChatType::YELL ||
                       data.type == ChatType::MONSTER_SAY || data.type == ChatType::MONSTER_YELL ||
                       data.type == ChatType::MONSTER_PARTY);
        // Party Chat Bubbles, a separate box beside the one for say and yell,
        // and read by nothing until now: party lines never floated over anyone
        // whichever way it was set. Off unless asked for, as it ships, since a
        // five-person group talking is a lot of text over the fight.
        if (!bubble && (data.type == ChatType::PARTY || data.type == ChatType::RAID)) {
            bubble = addons::storedCVarValue("chatBubblesParty", "0") != "0";
        }
        if (bubble) {
            bool isYell = (data.type == ChatType::YELL || data.type == ChatType::MONSTER_YELL);
            owner_.chatBubbleCallbackRef()(data.senderGuid, data.message, isYell);
        }
    }

    // Log the message
    std::string senderInfo;
    if (!data.senderName.empty()) {
        senderInfo = data.senderName;
    } else if (data.senderGuid != 0) {
        senderInfo = "Unknown-" + std::to_string(data.senderGuid);
    } else {
        senderInfo = "System";
    }

    std::string channelInfo;
    if (!data.channelName.empty()) {
        channelInfo = "[" + data.channelName + "] ";
    }

    LOG_DEBUG("[", getChatTypeString(data.type), "] ", channelInfo, senderInfo, ": ", data.message);

    // To the chat log as well, under the name the line shows.
    writeChatLogLine(data.type, data.message,
                     data.type == ChatType::WHISPER_INFORM && !data.receiverName.empty() ? data.receiverName
                                                                                          : data.senderName,
                     data.channelName, static_cast<uint32_t>(data.language), data.receiverName);

    // Fire CHAT_MSG_* addon events
    if (owner_.addonEventCallbackRef()) {
        // The language's *name*, not its id. ChatFrame_MessageEventHandler
        // does
        //
        //     if strlen(arg3) > 0 and arg3 ~= "Universal"
        //                          and arg3 ~= self.defaultLanguage then
        //         languageHeader = "["..arg3.."] "
        //
        // and GetDefaultLanguage answers "Common" or "Orcish". A number never
        // matches either, so every line in the player's own language was
        // printed with its id in front of it - guild chat in Common came out
        // as "[7] Name: hello". Universal is the id the file does not carry
        // and the word the test above looks for.
        const std::string lang = owner_.getLanguageName(static_cast<uint32_t>(data.language));
        char guidBuf[32];
        snprintf(guidBuf, sizeof(guidBuf), "0x%016llX", (unsigned long long)data.senderGuid);
        // arg8 is the channel's number and arg9 its name without the zone
        // after it, and both were sent empty. That is the whole of whether a
        // channel line is shown: the handler builds "CHANNEL"..arg8 and looks
        // it up in ChatTypeInfo, so a zero made every channel message
        // CHANNEL0, which no table has, and the message was dropped before it
        // reached the window. arg9 is what the frame matches against the
        // channels it carries when the zone id does not settle it.
        // A line id of its own, remembered against the sender. FrameXML builds
        // this into the player link on the name - "|Hplayer:Name:lineId:..." -
        // so a right-click on that name hands it back and the client is
        // expected to know which message it was. Zero for every line is one id
        // shared by all of them, which is what made Report Spam unofferable:
        // CanComplainChat is asked about the id before the entry is shown.
        const uint32_t lineId = nextChatLineId_++;
        if (data.senderGuid != 0) {
            chatLineSenders_.emplace_back(lineId, data.senderGuid);
            // Recent lines only. A session's chat has no end, and a message old
            // enough to have fallen out of this is old enough not to be
            // reportable.
            constexpr size_t kRememberedLines = 512;
            while (chatLineSenders_.size() > kRememberedLines)
                chatLineSenders_.pop_front();
        }
        // The channel's slot by the name on the line (0x00509dd0): arg4 is
        // the name numbered - "1. Allgemein - Dalaran", which the frame puts
        // in brackets in front of the line - and arg7 to arg10 are the
        // slot's zone channel id, number, name and instance. The frame finds
        // a zone channel by the id and any other by the name, so a line for a
        // channel with no slot reaches no frame, as in the client.
        const chat_channels::Slot* chanSlot =
            data.channelName.empty() ? nullptr : channelSlots_.byName(data.channelName);
        const std::string numberedChannel = data.channelName.empty()
            ? std::string() : chat_channels::numberedName(chanSlot, data.channelName);
        // arg5 is the *target*, not the sender. It was the sender's own name,
        // which is only read by CHANNEL_NOTICE_USER - and read as "there are
        // two names in this notice", so every kick and ban was formatted as
        // though someone had done it to someone else. receiverName is what the
        // packet carries for it, empty on an ordinary message.
        //
        // arg6 is the flag beside the name: FrameXML looks up CHAT_FLAG_<arg6>
        // and prints <Away>, <Busy> or <GM>. The tag is on the packet and was
        // being dropped, so nothing ever showed as away or as a game master.
        // AzerothCore's values are a bitmask - AFK 0x01, DND 0x02, GM 0x04,
        // COM 0x08, DEV 0x10 - and the interface takes one word, so the most
        // significant is the one to name.
        const char* chatFlag = (data.chatTag & 0x04) ? "GM"
                             : (data.chatTag & 0x10) ? "DEV"
                             : (data.chatTag & 0x02) ? "DND"
                             : (data.chatTag & 0x01) ? "AFK"
                                                     : "";
        // arg2 is the name the interface prints, and for an outgoing whisper
        // that is the person written to rather than the one writing:
        // CHAT_WHISPER_INFORM_GET is "To %s: " and takes the same argument
        // CHAT_WHISPER_GET does. Passing the sender there addressed every
        // whisper the player sent to the player themselves.
        const std::string& shownName =
            (data.type == ChatType::WHISPER_INFORM && !data.receiverName.empty())
                ? data.receiverName
                : data.senderName;
        const std::string eventName = chatEventNameFor(data.type, shownName);
        // Says a line reached the interface at all. A blank chat window is
        // either nothing arriving or something arriving and not being drawn,
        // and those have opposite causes with the same appearance.
        //
        // Once per event name, not once every two seconds: what is being
        // asked is whether a SAY, a YELL, a WHISPER reaches the interface,
        // and the second SAY answers nothing the first did. Rate limiting it
        // wrote one of these every two seconds for as long as anyone in the
        // zone was talking.
        {
            static std::set<std::string> saidFor;
            if (saidFor.size() < 32 && saidFor.insert(eventName).second) {
                LOG_WARNING("Chat: fired ", eventName, " to the interface from '",
                            data.senderName, "' - said once per event name");
            }
        }
        owner_.addonEventCallbackRef()(eventName, {
            data.message,
            shownName,
            lang,
            numberedChannel,
            data.receiverName,
            chatFlag,
            std::to_string(chanSlot ? chanSlot->zoneChannelId : 0u),
            std::to_string(chanSlot ? chanSlot->number : 0),
            chanSlot ? chanSlot->name : std::string(),
            std::to_string(chanSlot ? chanSlot->instanceId : 0u),
            std::to_string(lineId),
            guidBuf
        });
    }
}

void ChatHandler::sendTextEmote(uint32_t textEmoteId, uint64_t targetGuid) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    auto packet = TextEmotePacket::build(textEmoteId, targetGuid);
    owner_.getSocket()->send(packet);
}

std::string ChatHandler::knownPlayerName(uint64_t guid) const {
    if (guid == 0) return {};
    if (const std::string& name = owner_.lookupName(guid); !name.empty()) return name;
    for (const auto& member : owner_.getPartyData().members) {
        if (member.guid == guid && !member.name.empty()) return member.name;
    }
    for (const auto& member : owner_.getGuildRoster().members) {
        if (member.guid == guid && !member.name.empty()) return member.name;
    }
    return {};
}

TextSubject ChatHandler::monsterLineSubject(const MessageChatData& data) {
    // Addressed to nobody, or to us: the reader, which is who a line with no
    // one else in it is for.
    if (data.receiverGuid == 0 || data.receiverGuid == owner_.getPlayerGuid()) {
        return textSubjectFor(owner_);
    }
    TextSubject subject;
    if (isPlayerGuid(data.receiverGuid)) {
        if (std::string name = knownPlayerName(data.receiverGuid); !name.empty()) {
            subject.name = std::move(name);
        }
        // Class, race and gender come with the name query's answer. Unknown,
        // they stay neutral rather than borrowing the reader's.
        if (const uint8_t cls = owner_.lookupPlayerClass(data.receiverGuid)) {
            subject.className = getClassName(static_cast<Class>(cls));
        }
        if (const uint8_t race = owner_.lookupPlayerRace(data.receiverGuid)) {
            subject.raceName = getRaceName(static_cast<Race>(race));
        }
        const uint8_t gender = owner_.lookupPlayerGender(data.receiverGuid);
        if (gender == 0) subject.gender = Gender::MALE;
        else if (gender == 1) subject.gender = Gender::FEMALE;
    } else if (!data.receiverName.empty()) {
        subject.name = data.receiverName;
    } else if (const std::string& name = owner_.lookupName(data.receiverGuid); !name.empty()) {
        subject.name = name;
    }
    return subject;
}

void ChatHandler::flushChatAwaitingName(uint64_t guid) {
    if (chatAwaitingName_.empty() || guid == 0) return;
    std::vector<MessageChatData> ready;
    for (auto it = chatAwaitingName_.begin(); it != chatAwaitingName_.end();) {
        if (it->guid == guid) {
            ready.push_back(std::move(it->data));
            it = chatAwaitingName_.erase(it);
        } else {
            ++it;
        }
    }
    // Delivered outside the walk: each one runs the whole chat path, which can
    // hold another line of its own.
    for (auto& data : ready) deliverChatMessage(std::move(data), /*alreadyWaited=*/true);
}

void ChatHandler::expireChatAwaitingName() {
    if (chatAwaitingName_.empty()) return;
    const double now = core::appTimeSeconds();
    std::vector<MessageChatData> ready;
    for (auto it = chatAwaitingName_.begin(); it != chatAwaitingName_.end();) {
        if (now >= it->deadline) {
            LOG_WARNING("Chat: no name for ", formatChatLogGuid(it->guid),
                        " within ", kNameWaitSeconds, "s - showing the line without one");
            ready.push_back(std::move(it->data));
            it = chatAwaitingName_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto& data : ready) deliverChatMessage(std::move(data), /*alreadyWaited=*/true);
}

void ChatHandler::handleTextEmote(network::Packet& packet) {
    const bool legacyFormat = isClassicLikeExpansion();
    TextEmoteData data;
    if (!TextEmoteParser::parse(packet, data, legacyFormat)) {
        LOG_WARNING("Failed to parse SMSG_TEXT_EMOTE");
        return;
    }

    // The player's own emote, as the server echoes it, is the line the chat
    // shows for it - "You wave at Bob." This returned here, on the reading that
    // the line had been made when the emote was sent. It is not any more: the
    // interface sends emotes through DoEmote, which makes no line, so the
    // player's own emotes never appeared at all.
    if (data.senderGuid == owner_.getPlayerGuid() && data.senderGuid != 0) {
        const std::string* target = data.targetName.empty() ? nullptr : &data.targetName;
        std::string text =
            rendering::AnimationController::getSelfEmoteTextByDbcId(data.textEmoteId, target);
        if (text.empty()) return;
        const Character* ac = owner_.getActiveCharacter();
        MessageChatData chatMsg;
        chatMsg.type = ChatType::TEXT_EMOTE;
        chatMsg.language = ChatLanguage::UNIVERSAL;
        chatMsg.senderGuid = data.senderGuid;
        chatMsg.senderName = ac ? ac->name : std::string{};
        chatMsg.message = std::move(text);
        addLocalChatMessage(chatMsg);
        return;
    }

    std::string senderName = owner_.lookupName(data.senderGuid);
    // An NPC's name from the creature queries, when it is not in sight to be
    // read off. Asked of the player name query, as it was, it is never
    // answered, and the emote was dropped for want of a name.
    if (senderName.empty() && !isPlayerGuid(data.senderGuid)) {
        if (const uint32_t entry = creatureEntryOf(data.senderGuid)) {
            senderName = owner_.getCachedCreatureName(entry);
            if (senderName.empty()) owner_.queryCreatureInfo(entry, data.senderGuid);
        }
    }
    // SMSG_TEXT_EMOTE is the narrated chat line. The server sends the actual
    // visual separately in SMSG_EMOTE; replaying an animation here restarts
    // one-shots and replaces correctly resolved STATE_* loops such as /dance.
    if (senderName.empty()) {
        const auto& partyData = owner_.getPartyData();
        for (const auto& member : partyData.members) {
            if (member.guid == data.senderGuid && !member.name.empty()) {
                senderName = member.name;
                break;
            }
        }
    }

    if (senderName.empty()) {
        if (isPlayerGuid(data.senderGuid)) owner_.queryPlayerName(data.senderGuid);
        LOG_DEBUG("Deferred chat text for unresolved SMSG_TEXT_EMOTE sender=0x",
                  std::hex, data.senderGuid, std::dec,
                  " emoteId=", data.textEmoteId);
        return;
    }

    const std::string* targetPtr = data.targetName.empty() ? nullptr : &data.targetName;
    std::string emoteText = rendering::AnimationController::getEmoteTextByDbcId(data.textEmoteId, senderName, targetPtr);
    if (emoteText.empty()) {
        emoteText = data.targetName.empty()
            ? senderName + " performs an emote."
            : senderName + " performs an emote at " + data.targetName + ".";
    }

    MessageChatData chatMsg;
    chatMsg.type = ChatType::TEXT_EMOTE;
    // Universal: an emote is seen, not spoken, and anything else is printed
    // as a language header in front of it.
    chatMsg.language = ChatLanguage::UNIVERSAL;
    chatMsg.senderGuid = data.senderGuid;
    chatMsg.senderName = senderName;
    chatMsg.message = emoteText;

    addLocalChatMessage(chatMsg);

    // And the emote's own voice line, which the client asks of the sender at
    // the same moment it narrates it: EmotesTextSound picks the clip by the
    // emote text and the sender's race and sex, and the sender's own sound
    // path plays it - gates for emote and pet sounds included.
    if (owner_.emoteSoundCallbackRef()) owner_.emoteSoundCallbackRef()(data.senderGuid, data.textEmoteId);

    LOG_INFO("TEXT_EMOTE from ", senderName, " (emoteId=", data.textEmoteId, ")");
}

const std::vector<chat_channels::Row>& ChatHandler::channelRows() const {
    if (channelRowsLoaded_) return channelRows_;
    auto* am = owner_.services().assetManager;
    // Not an attempt: the assets are not there to read yet, and a caller can
    // reach this before they are.
    if (!am || !am->isInitialized()) return channelRows_;
    channelRowsLoaded_ = true;
    auto dbc = am->loadDBC("ChatChannels.dbc");
    if (!dbc || !dbc->isLoaded()) {
        LOG_WARNING("ChatChannels.dbc not available - zone channels cannot be joined");
        return channelRows_;
    }
    // ID, Flags, FactionGroup, then the two localized strings. The layout
    // says where they are; without one the column count does, since the
    // installed layouts file is a copy taken at extraction and may predate
    // this table: 37 columns is sixteen locales and a flags column each
    // (TBC, WotLK - 0x0063a3f0 insists on 37), 21 is vanilla's eight, and
    // fewer is the unlocalized 4.x shape.
    const uint32_t fields = dbc->getFieldCount();
    uint32_t idField = 0, flagsField = 1, nameField = 3;
    uint32_t shortcutField = fields >= 37 ? 20u : fields >= 21 ? 12u : 4u;
    if (const auto* layouts = pipeline::getActiveDBCLayout()) {
        if (const auto* l = layouts->getLayout("ChatChannels")) {
            const auto pick = [&](const char* name, uint32_t& out) {
                const uint32_t f = l->tryField(name);
                if (f != 0xFFFFFFFFu && f < fields) out = f;
            };
            pick("ID", idField);
            pick("Flags", flagsField);
            pick("Name", nameField);
            pick("Shortcut", shortcutField);
        }
    }
    if (shortcutField >= fields || nameField >= fields) {
        LOG_WARNING("ChatChannels.dbc has ", fields, " columns, too few for its names");
        return channelRows_;
    }
    // Each string from its locale block, whichever column the client filled.
    const uint32_t width = chat_channels::localeWidth(fields);
    std::string summary;
    for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
        const auto column = [&](uint32_t f) {
            return f < fields ? dbc->getString(i, f) : std::string();
        };
        chat_channels::Row row;
        row.id = dbc->getUInt32(i, idField);
        row.flags = dbc->getUInt32(i, flagsField);
        row.name = chat_channels::localizedText(column, nameField, std::min(width, fields - nameField));
        row.shortcut = chat_channels::localizedText(column, shortcutField,
                                                    std::min(width, fields - shortcutField));
        // A row without both names cannot be joined: there is nothing to ask
        // for it by and nothing to send.
        if (row.id == 0 || row.shortcut.empty() || row.name.empty()) {
            LOG_WARNING("ChatChannels.dbc row ", row.id, " has no name or shortcut - skipped");
            continue;
        }
        summary += " " + std::to_string(row.id) + "='" + row.shortcut + "'/'" + row.name + "'";
        channelRows_.push_back(std::move(row));
    }
    LOG_WARNING("ChatChannels: ", channelRows_.size(), " rows from ", fields, " columns:", summary);
    return channelRows_;
}

std::vector<std::pair<std::string, int>> ChatHandler::defaultWindowChannels() const {
    // The rows the client joins by itself, which 0x00508320 puts in the
    // first window by shortcut and id when there is no chat cache - and Guild
    // Recruitment, which 0x00507d30 adds when it joins it.
    std::vector<std::pair<std::string, int>> out;
    for (const auto& r : channelRows()) {
        if (r.flags & (chat_channels::kFlagInitial | chat_channels::kFlagGuildRecruitment))
            out.emplace_back(r.shortcut, static_cast<int>(r.id));
    }
    return out;
}

chat_channels::Place ChatHandler::currentChannelPlace() const {
    chat_channels::Place place;
    // The real zone's text, which the client keeps at 0x00bd0780 and
    // 0x00507a80 reads: "Dalaran", not the district the player stands in.
    auto* renderer = core::Application::getInstance().getRenderer();
    const auto* texts = renderer ? renderer->getZoneTexts() : nullptr;
    if (!texts || texts->realZone.empty()) return place;
    place.zone = texts->realZone;
    // The zone's own AreaTable row (0x00bd080c is the zone's id, as
    // 0x0078f020 hands it to 0x005204c0).
    place.zoneIsCity = (owner_.getAreaFlags(texts->zoneId) & chat_channels::kAreaFlagCity) != 0;
    place.capitals = owner_.getCapitalsAreaName();
    return place;
}

void ChatHandler::refreshJoinedChannels() {
    joinedChannels_.clear();
    for (const auto& s : channelSlots_.all()) {
        if (s.number != 0 && !s.pending) joinedChannels_.push_back(s.name);
    }
}

std::optional<chat_channels::Join> ChatHandler::joinChannel(const std::string& channelName,
                                                            const std::string& password,
                                                            bool byLogin) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return std::nullopt;
    // Never a channel without a name - the client's interface refuses one
    // before it gets here, and the server would make one called "".
    if (channelName.empty()) {
        LOG_WARNING("Not joining a channel with no name");
        return std::nullopt;
    }
    auto join = chat_channels::resolveJoin(channelRows(), channelName, currentChannelPlace());
    if (!join) {
        LOG_WARNING("Not joining channel '", channelName, "': no zone to name it after yet");
        return std::nullopt;
    }
    if (join->name.empty()) {
        LOG_WARNING("Not joining channel '", channelName, "': its row has no name");
        return std::nullopt;
    }
    // A zone channel goes without the password typed with it (0x00507a80
    // clears it on a row match).
    const std::string pass = join->zoneChannelId != 0 ? std::string() : password;
    // A slot already held - by id for a zone channel - is a channel already
    // asked for or joined, and the client sends nothing again. Otherwise the
    // slot is taken now, which is what fixes the channel's number.
    const bool held = join->zoneChannelId != 0 ? channelSlots_.byZoneId(join->zoneChannelId) != nullptr
                                               : channelSlots_.byName(join->name) != nullptr;
    if (!held) {
        if (!channelSlots_.reserve(join->name, join->zoneChannelId)) {
            // reserve() also refuses a name or id already held; "held" was
            // checked above, so this is the slots being full.
            LOG_WARNING("Not joining channel '", join->name, "': all ",
                        chat_channels::kMaxSlots, " channel slots are taken");
            return std::nullopt;
        }
        // The login join sends 1 in the first byte and the interface's 0.
        const uint8_t byZone = byLogin ? 1 : 0;
        auto packet = owner_.getPacketParsers()
            ? owner_.getPacketParsers()->buildJoinChannel(join->name, pass, join->zoneChannelId, byZone, 0)
            : JoinChannelPacket::build(join->name, pass, join->zoneChannelId, byZone, 0);
        owner_.getSocket()->send(packet);
        LOG_WARNING("Requesting to join channel: '", join->name, "' id=", join->zoneChannelId,
                 " (asked for as '", channelName, "')");
    }
    return join;
}

void ChatHandler::requestChannelList(const std::string& channelName) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    // The channel name and nothing else. SMSG_CHANNEL_LIST was parsed all
    // along and nothing ever asked for one, so the roster only existed in
    // theory.
    network::Packet packet(wireOpcode(Opcode::CMSG_CHANNEL_LIST));
    packet.writeString(channelName);
    owner_.getSocket()->send(packet);
    LOG_INFO("Requesting member list for channel: ", channelName);
}

std::string ChatHandler::resolveChannelArg(const std::string& channel) const {
    if (channel.empty()) return {};
    // 0x0076f0d0 reads the number off the front, so "2" and "2nd" are both 2.
    const int number = std::atoi(channel.c_str());
    if (number == 0) return channel;
    const auto* s = channelSlots_.joinedByNumber(number);
    return s ? s->name : std::string();
}

void ChatHandler::setChannelWatch(const std::string& channel) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    const std::string name = resolveChannelArg(channel);
    if (name.empty()) return;
    network::Packet packet(wireOpcode(Opcode::CMSG_SET_CHANNEL_WATCH));
    packet.writeString(name);
    owner_.getSocket()->send(packet);
}

void ChatHandler::clearChannelWatch() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    network::Packet packet(wireOpcode(Opcode::CMSG_CLEAR_CHANNEL_WATCH));
    owner_.getSocket()->send(packet);
}

void ChatHandler::channelSilenceAll(const std::string& channel, const std::string& member,
                                    bool silence) {
    const std::string name = resolveChannelArg(channel);
    if (name.empty()) return;
    auto isNamed = [&](const char* group) {
        return name.size() == std::strlen(group) &&
               std::equal(group, group + std::strlen(group), name.begin(),
                          [](char a, char b) {
                              return std::tolower(static_cast<unsigned char>(a)) ==
                                     std::tolower(static_cast<unsigned char>(b));
                          });
    };
    if (isNamed("Party") || isNamed("Raid") || isNamed("Battleground")) return;
    // 0x004fe330 refuses a member name longer than 48.
    if (member.size() > 48) return;
    owner_.channelModeration(silence ? Opcode::CMSG_CHANNEL_SILENCE_ALL
                                     : Opcode::CMSG_CHANNEL_UNSILENCE_ALL,
                             name, member, true);
}

std::vector<std::string> ChatHandler::serverChannelNames() const {
    std::vector<std::string> names;
    auto* renderer = core::Application::getInstance().getRenderer();
    const auto* texts = renderer ? renderer->getZoneTexts() : nullptr;
    if (!texts || texts->zoneId == 0) return names;
    // AreaTable's flags, tested with 0x8 here (0x004ff277) rather than the
    // 0x100 the joining code tests.
    constexpr uint32_t kAreaFlagServerChannelCity = 0x8;
    const bool city = (owner_.getAreaFlags(texts->zoneId) & kAreaFlagServerChannelCity) != 0;
    for (const auto& r : channelRows()) {
        if ((r.flags & chat_channels::kFlagCityOnly) && !city) continue;
        names.push_back(r.shortcut.empty() ? r.name : r.shortcut);
    }
    return names;
}

void ChatHandler::leaveChannel(const std::string& channelName) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    // A number names the joined channel with that number (0x004fe160); one
    // that names none leaves nothing.
    std::string name = channelName;
    if (!name.empty() && std::all_of(name.begin(), name.end(),
                                     [](unsigned char c) { return std::isdigit(c) != 0; })) {
        const auto* s = channelSlots_.joinedByNumber(std::atoi(name.c_str()));
        if (!s) return;
        name = s->name;
    }
    // A shortcut is its zone channel, by id - the name it has now being
    // whichever zone the server last put the player in.
    const auto join = chat_channels::resolveJoin(channelRows(), name, currentChannelPlace());
    if (!join) return;
    chat_channels::Slot* slot = join->zoneChannelId != 0 ? channelSlots_.byZoneId(join->zoneChannelId)
                                                         : channelSlots_.byName(join->name);
    // Only a joined channel is left (0x005093f0): a slot still waiting for
    // the server has nothing to leave yet.
    if (!slot || slot->pending) return;
    auto packet = owner_.getPacketParsers()
        ? owner_.getPacketParsers()->buildLeaveChannel(slot->name, slot->zoneChannelId)
        : LeaveChannelPacket::build(slot->name, slot->zoneChannelId);
    owner_.getSocket()->send(packet);
    slot->leaving = true;
    // Not joined by itself at the next login either.
    zoneChannelMask_ &= ~chat_channels::maskBit(slot->zoneChannelId);
    LOG_INFO("Requesting to leave channel: '", slot->name, "' id=", slot->zoneChannelId);
}

std::string ChatHandler::getChannelByIndex(int index) const {
    const auto* s = channelSlots_.joinedByNumber(index);
    return s ? s->name : std::string();
}

int ChatHandler::getChannelIndex(const std::string& channelName) const {
    // Any slot of that name, as 0x00509dd0 looks the channel of a line up.
    const auto* s = channelSlots_.byName(channelName);
    return s ? s->number : 0;
}

void ChatHandler::fireChannelNotice(ChatType type, const std::string& word, const std::string& channelName,
                                    const std::string& player, const std::string& target,
                                    uint64_t senderGuid) {
    const chat_channels::Slot* slot = channelSlots_.byName(channelName);
    const std::string numbered = chat_channels::numberedName(slot, channelName);
    const int number = slot ? slot->number : 0;
    const uint32_t instance = slot ? slot->instanceId : 0;
    const bool user = type == ChatType::CHANNEL_NOTICE_USER;

    // The words are the interface's CHAT_<word>_NOTICE, which is what puts
    // "Channel beigetreten: [1. Allgemein - Dalaran]" on a German client. The
    // line kept here for the chat log and history is that text; what the
    // interface is told is the word, and it prints the line itself.
    std::string format;
    bool interfaceStrings = false;
    if (auto* addons = core::Application::getInstance().getAddonManager()) {
        if (auto* lua = addons->getLuaEngine()) {
            format = lua->globalText(("CHAT_" + word + "_NOTICE").c_str());
            interfaceStrings = !lua->globalText("CHAT_YOU_JOINED_NOTICE").empty();
        }
    }
    if (format.empty() && !interfaceStrings) {
        // No interface strings at all: enUS's, for want of them.
        if (word == "YOU_JOINED") format = "Joined Channel: |Hchannel:%d|h[%s]|h";
        else if (word == "YOU_CHANGED") format = "Changed Channel: |Hchannel:%d|h[%s]|h";
        else if (word == "YOU_LEFT" || word == "SUSPENDED") format = "Left Channel: |Hchannel:%d|h[%s]|h";
        else format = "|Hchannel:%d|h[%s]|h " + word;
    }
    MessageChatData msg;
    msg.type = type;
    msg.language = ChatLanguage::UNIVERSAL;
    msg.channelName = channelName;
    msg.senderName = player;
    msg.receiverName = target;
    msg.senderGuid = senderGuid;
    msg.message = format.empty() ? std::string()
                                 : chat_channels::noticeLine(format, user, word, number, numbered,
                                                             player, target, instance);
    if (!msg.message.empty()) {
        chatHistory_.push_back(msg);
        chatHistory_.back().uid = ++chatUidCounter_;
        if (chatHistory_.size() > maxChatHistory_) chatHistory_.pop_front();
        logChatMessage(msg, "channel");
    }
    LOG_WARNING("Channel notice ", word, " for '", channelName, "' (", numbered, ")");
    writeChatLogLine(type, word, player, channelName, 0, target);

    if (!owner_.addonEventCallbackRef()) return;
    char guidBuf[32] = "";
    if (senderGuid != 0)
        snprintf(guidBuf, sizeof(guidBuf), "0x%016llX", (unsigned long long)senderGuid);
    const uint32_t lineId = nextChatLineId_++;
    // 0x004fdbc0's arguments: the word, the player, no language, the channel
    // numbered, the second player, no flag, then the slot's zone channel id,
    // number, name and instance - all zero and empty for a channel with no
    // slot, which no chat frame matches and so does not print.
    owner_.addonEventCallbackRef()(chatEventNameFor(type, player), {
        word, player, "", numbered, target, "",
        std::to_string(slot ? slot->zoneChannelId : 0u),
        std::to_string(number),
        slot ? slot->name : std::string(),
        std::to_string(instance),
        std::to_string(lineId),
        guidBuf
    });
}

void ChatHandler::handleChannelNotify(network::Packet& packet) {
    ChannelNotifyData data;
    if (!ChannelNotifyParser::parse(packet, data)) {
        LOG_WARNING("Failed to parse SMSG_CHANNEL_NOTIFY");
        return;
    }
    namespace cc = chat_channels;
    const uint8_t type = static_cast<uint8_t>(data.notifyType);
    const auto nameOf = [&](uint64_t guid) {
        if (guid == 0) return std::string();
        std::string n = knownPlayerName(guid);
        if (n.empty()) owner_.queryPlayerName(guid);
        return n;
    };

    // The channel store's changes are also a notice.  0x00504840 rebuilds the
    // display list its rows come out of and, when its caller hands it a 1,
    // raises CHANNEL_UI_UPDATE with nothing at all (line 4032: the fire is
    // `FUN_0081b530(0x222, 0)`, and 0x222 = 546); the four callers that do hand
    // it one are the store's own join (0x00507880), pending/leaving
    // (0x00507900), leave (0x00507a30) and free (0x005093f0) routines, each of
    // which ends in 0x00505d40, which is the refresh with that 1.  The other
    // three call sites pass 0 - a query refreshing for itself - so the notice
    // belongs to the changes and not to the reading.  It is raised once per
    // change, however many rows one routine moved, and channelframe.lua:20,33
    // answers it with ChannelFrame_Update(), which reads the whole store back:
    // without it the Channels panel kept the list it had when it was opened.
    bool storeChanged = false;   // already told, for a notice that moves two parts
    const auto noteChannelStore = [&] {
        if (storeChanged || !owner_.addonEventCallbackRef()) return;
        storeChanged = true;
        owner_.addonEventCallbackRef()("CHANNEL_UI_UPDATE", {});
    };

    // 0x0050e120: every notice is a word handed to the interface, which
    // finds CHAT_<word>_NOTICE in its own strings. This client used to write
    // its own English sentence for each instead.
    switch (data.notifyType) {
        case ChannelNotifyType::PLAYER_JOINED:
        case ChannelNotifyType::PLAYER_LEFT: {
            // CHAT_MSG_CHANNEL_JOIN and _LEAVE, for a channel that is joined
            // (0x004fdee0) and with the player's name. The client holds such
            // a line until the name is known; here one without a name is
            // dropped and the name asked for.
            const auto* slot = channelSlots_.byName(data.channelName);
            if (!slot || slot->pending) return;
            const std::string who = nameOf(data.senderGuid);
            if (who.empty()) return;
            const ChatType t = data.notifyType == ChannelNotifyType::PLAYER_JOINED
                ? ChatType::CHANNEL_JOIN : ChatType::CHANNEL_LEAVE;
            writeChatLogLine(t, "", who, data.channelName, 0, "");
            if (!owner_.addonEventCallbackRef()) return;
            char guidBuf[32];
            snprintf(guidBuf, sizeof(guidBuf), "0x%016llX", (unsigned long long)data.senderGuid);
            owner_.addonEventCallbackRef()(chatEventNameFor(t, who), {
                "", who, "", cc::numberedName(slot, data.channelName), "", "",
                std::to_string(slot->zoneChannelId), std::to_string(slot->number), slot->name,
                std::to_string(slot->instanceId), std::to_string(nextChatLineId_++), guidBuf});
            return;
        }
        case ChannelNotifyType::MODE_CHANGE:
            // A member's flags, which 0x0050cfc0 applies to the roster; no line.
            LOG_DEBUG("Channel mode change in ", data.channelName);
            return;
        default:
            break;
    }

    // 0x0050e120 case 0x18: an invite is a name request before it is a popup.
    // Its guid is resolved first, and the whole notice is dropped - the line
    // with it - on any of three tests: the inviter is on the ignore list
    // (0x006b52e0), the guid names no player, or an invite is already waiting
    // on a name (the client keeps one pending channel in the 0x80 bytes at
    // DAT_00b75950, and its own test for that is the first byte of it).
    if (data.notifyType == ChannelNotifyType::INVITE) {
        const uint64_t inviter = data.senderGuid;
        if (inviter == 0 || (inviter >> 32) & 0xF0000000ULL ||
            owner_.ignoreListGuidsRef().count(inviter) || !pendingInviteChannel_.empty())
            return;
        if (const std::string who = knownPlayerName(inviter); !who.empty()) {
            // Known already, so the popup goes up inside the case - which is
            // before the handler writes its line, the line being delivered
            // after the switch. CHANNEL_INVITE_REQUEST carries the channel then
            // the inviter (0x009e1ad0 is "%s%s"), and staticpopup.lua's
            // CHAT_CHANNEL_INVITE hands its arg1 straight to
            // JoinPermanentChannel, so the channel is the joinable name.
            if (owner_.addonEventCallbackRef())
                owner_.addonEventCallbackRef()("CHANNEL_INVITE_REQUEST", {data.channelName, who});
        } else {
            // The request carries the callback (0x004fb5c0), so this waits for
            // the answer that asked for it and not for whichever name lands.
            pendingInviteGuid_ = inviter;
            pendingInviteChannel_ = data.channelName;
            owner_.queryPlayerName(inviter);
        }
    }

    cc::Notice notice;
    uint32_t slotId = 0;  // the id a freed slot is found by: YOU_JOINED's and YOU_LEFT's only
    if (data.notifyType == ChannelNotifyType::YOU_JOINED) {
        // Joined while its slot was already joined is the server moving the
        // player into another zone's channel of the same row: YOU_CHANGED.
        const cc::Slot* before = channelSlots_.find(data.zoneChannelId, data.channelName);
        notice = cc::noticeFor(type, before && !before->pending);
        const bool newly = channelSlots_.joined(data.zoneChannelId, data.channelName,
                                                data.instanceId, data.channelFlags);
        if (newly) zoneChannelMask_ |= cc::maskBit(data.zoneChannelId);
        slotId = data.zoneChannelId;
        // 0x00507880's own test is the record's in-use flag: the refresh runs
        // when the store already had this channel - the pending slot a join
        // request left behind - and not at all for a name it never heard of.
        if (before != nullptr || newly) noteChannelStore();
    } else if (data.notifyType == ChannelNotifyType::YOU_LEFT) {
        // A constant channel the server takes away with the zone - Trade on
        // leaving a city - is SUSPENDED and keeps its number; one the player
        // left is gone.
        const cc::Slot* before = channelSlots_.find(data.zoneChannelId, data.channelName);
        notice = cc::noticeFor(type, false, before != nullptr, before && before->leaving, data.constant);
        channelSlots_.left(data.zoneChannelId, data.channelName);
        slotId = data.zoneChannelId;
        ownedChannels_.erase(data.channelName);
        // Case 0x03 hands the row to 0x00507900, which marks it leaving and
        // ends in the refresh - and that is inside the case, before the line
        // the switch writes on the way out.  The channel suspending with its
        // number kept (SUSPENDED) is the same leg.
        noteChannelStore();
    } else {
        notice = cc::noticeFor(type);
    }
    if (data.notifyType == ChannelNotifyType::OWNER_CHANGED) {
        // The guid is the new owner's: AzerothCore's Channel::MakeOwnerChanged
        // writes _ownerGUID into it. Kept so IsDisplayChannelOwner can answer.
        if (data.senderGuid != 0 && data.senderGuid == owner_.getPlayerGuid())
            ownedChannels_.insert(data.channelName);
        else
            ownedChannels_.erase(data.channelName);
    }
    refreshJoinedChannels();
    if (!notice.word) {
        LOG_DEBUG("Channel notify type ", static_cast<int>(type), " for channel ", data.channelName);
        return;
    }
    // The wrong-password notice is also an order to ask again: case 0x04 fires
    // CHANNEL_PASSWORD_REQUEST with the channel name (0x009e0e50 is "%s") from
    // the same case that prints WRONG_PASSWORD, and uiparent.lua answers that
    // name with StaticPopup_Show("CHAT_CHANNEL_PASSWORD"), whose OK button runs
    // ChatChannelPasswordHandler -> JoinPermanentChannel(name, password, ...).
    // Without the name the player is told the password was wrong and given no
    // way to enter another, because nothing else shows that dialog.
    if (data.notifyType == ChannelNotifyType::WRONG_PASSWORD && owner_.addonEventCallbackRef())
        owner_.addonEventCallbackRef()("CHANNEL_PASSWORD_REQUEST", {data.channelName});
    const std::string player = !data.playerName.empty() ? data.playerName : nameOf(data.senderGuid);
    fireChannelNotice(notice.user ? ChatType::CHANNEL_NOTICE_USER : ChatType::CHANNEL_NOTICE,
                      notice.word, data.channelName, player, nameOf(data.targetGuid), data.senderGuid);
    if (notice.freesSlot) {
        channelSlots_.clear(slotId, data.channelName);
        ownedChannels_.erase(data.channelName);
        refreshJoinedChannels();
        // The notices that free a slot are the ones 0x0050e120 hands to
        // 0x00507a30 - `if (local_18 != 0)` at 11354, past the line it wrote at
        // 11353 - and that routine ends in the refresh too.  For a leave this
        // is the second of the two legs and the notice was already told when
        // the row moved.
        noteChannelStore();
    }
}

void ChatHandler::onChannelInviteNameKnown(uint64_t guid) {
    // The callback 0x0050e120 case 0x18 registers with the name request is
    // 0x004fb5c0, and the request answers it with the guid it asked for - so an
    // answer about somebody else cannot be the answer this invite was waiting
    // for, and an answer that found no name leaves the invite pending exactly as
    // the client's does (its 0x00b75950 is cleared only on the way out here).
    if (pendingInviteChannel_.empty() || pendingInviteGuid_ != guid) return;
    const std::string who = knownPlayerName(guid);
    if (who.empty()) return;
    const std::string channel = pendingInviteChannel_;
    pendingInviteChannel_.clear();
    pendingInviteGuid_ = 0;
    if (owner_.addonEventCallbackRef())
        owner_.addonEventCallbackRef()("CHANNEL_INVITE_REQUEST", {channel, who});
}

void ChatHandler::autoJoinDefaultChannels() {
    // A new world: no channels yet, and the zone channels to be asked for
    // once the player has a zone (0x0050edd0 clears the state at login and
    // 0x00508320 arms the join).
    channelSlots_.reset();
    joinedChannels_.clear();
    ownedChannels_.clear();
    // An invite waiting on a name asked for in the old world is never answered
    // in this one, and holding it would drop every invite of the new one.
    pendingInviteChannel_.clear();
    pendingInviteGuid_ = 0;
    zoneChannelMask_ = 0;
    loginChannelJoinArmed_ = true;
    loginChannelJoinDone_ = false;
}

void ChatHandler::updateLoginChannelJoin() {
    if (!loginChannelJoinArmed_ || loginChannelJoinDone_) return;
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    // Not on a flight path: 0x005204c0 and the flag update both skip the
    // join while the taxi flag is up, and the flag dropping is what runs it.
    // Not while a cinematic plays or waits for the world: its end runs it
    // (0x0051d7c0). Not before the zone has a name: every zone channel is
    // named after it, and 0x00508090 waits for the text.
    const chat_channels::Place place = currentChannelPlace();
    if (!chat_channels::loginJoinMayRun(owner_.isOnTaxiFlight(), owner_.inCinematic(), owner_.cinematicWaiting(),
                                        !place.zone.empty()))
        return;
    const auto& rows = channelRows();
    if (rows.empty()) {
        if (channelRowsLoaded_) {
            loginChannelJoinDone_ = true;
            LOG_WARNING("No zone channels joined: ChatChannels.dbc gave no rows");
        }
        return;
    }
    loginChannelJoinDone_ = true;

    // The client's mask comes from its chat cache, and a character without
    // one gets every row flagged initial. This client keeps no chat cache;
    // its settings are the switches it has always had, each taking its row
    // out of the initial set.
    uint32_t mask = chat_channels::initialMask(rows);
    const auto drop = [&](bool on, const char* legacy) {
        if (on) return;
        if (const auto* r = chat_channels::rowForLegacyName(rows, legacy))
            mask &= ~chat_channels::maskBit(r->id);
    };
    drop(chatAutoJoin.general, "General");
    drop(chatAutoJoin.trade, "Trade");
    drop(chatAutoJoin.localDefense, "LocalDefense");
    drop(chatAutoJoin.lfg, "LookingForGroup");
    zoneChannelMask_ = mask;
    LOG_WARNING("Joining zone channels for '", place.zone, "'", place.zoneIsCity ? " (a city)" : "",
                ": mask=0x", std::hex, mask, std::dec);
    // In the DBC's order, by shortcut, as 0x00508090 does - which is what
    // numbers them: General 1, Trade 2, LocalDefense 3, LookingForGroup 4.
    // A row a zone has no channel for (Trade outside a city) is still asked
    // for and keeps its number; the server answers when the player reaches
    // a zone that has it.
    for (const auto& r : rows) {
        if (mask & chat_channels::maskBit(r.id)) joinChannel(r.shortcut, "", true);
    }

    // Guild Recruitment (0x00507d30, run at the end of the login join): with
    // the interface's option on, joined by a player in no guild. It has no
    // switch of this client's own; the CVar is the option.
    if (addons::storedCVarValue("guildRecruitmentChannel", "0") != "0" && !owner_.isInGuild()) {
        for (const auto& r : rows) {
            if (r.flags & chat_channels::kFlagGuildRecruitment) {
                joinChannel(r.shortcut);
                break;
            }
        }
    }
}

void ChatHandler::addLocalChatMessage(const MessageChatData& msg) {
    chatHistory_.push_back(msg);
    chatHistory_.back().uid = ++chatUidCounter_;
    if (chatHistory_.size() > maxChatHistory_) {
        chatHistory_.pop_front();
    }
    logChatMessage(msg, "local");

    fireChatEvent(msg);
}

void ChatHandler::fireChatEvent(const MessageChatData& msg) {
    const Character* ac = owner_.getActiveCharacter();
    // A channel line with no sender at all - the defense notices - is from no
    // one, and chatframe.lua prints it without a name only when it has none.
    const bool fromNoOne = msg.type == ChatType::CHANNEL && msg.senderGuid == 0 && msg.senderName.empty();
    std::string senderName = (msg.senderName.empty() && !fromNoOne)
        ? (ac ? ac->name : std::string{}) : msg.senderName;
    // arg2 is the name the interface prints, and for a whisper the player
    // sent that is who it went to, not who wrote it: CHAT_WHISPER_INFORM_GET
    // is "To %s: " and reads the same argument CHAT_WHISPER_GET does. The
    // other announce path already does this; this one did not, and this is
    // the path an outgoing whisper actually takes - the server's echo is
    // dropped in favour of the local row made on send - so every whisper the
    // player sent came back addressed to the player.
    const std::string& shownName =
        (msg.type == ChatType::WHISPER_INFORM && !msg.receiverName.empty())
            ? msg.receiverName
            : senderName;
    writeChatLogLine(msg.type, msg.message, shownName, msg.channelName, static_cast<uint32_t>(msg.language),
                     msg.receiverName);
    if (!owner_.addonEventCallbackRef()) return;
    const std::string eventName = chatEventNameFor(msg.type, shownName);
    char guidBuf[32];
    snprintf(guidBuf, sizeof(guidBuf), "0x%016llX",
             (unsigned long long)(msg.senderGuid != 0 || fromNoOne ? msg.senderGuid : owner_.getPlayerGuid()));
    // The name, for the reason handleMessageChat gives: a number here is
    // printed as a language header in front of every line.
    {
        // Once per event name, for the reason the remote path above gives.
        static std::set<std::string> saidForLocal;
        if (saidForLocal.size() < 32 && saidForLocal.insert(eventName).second) {
            LOG_WARNING("Chat: fired ", eventName, " to the interface (local)"
                        " - said once per event name");
        }
    }
    // The channel's index, which the chat frame builds "CHANNEL"..arg8 out of
    // and looks up in ChatTypeInfo - a zero there is CHANNEL0, which no table
    // has, and the line is dropped before it reaches the window. This is the
    // one thing the callback that used to announce these as well did better,
    // and it is here now so nothing was lost when that went.
    //
    // The same slot-shaped arguments the server's lines carry (0x00509dd0).
    const chat_channels::Slot* chanSlot =
        msg.channelName.empty() ? nullptr : channelSlots_.byName(msg.channelName);
    owner_.addonEventCallbackRef()(eventName, {
        msg.message, shownName,
        owner_.getLanguageName(static_cast<uint32_t>(msg.language)),
        msg.channelName.empty() ? std::string() : chat_channels::numberedName(chanSlot, msg.channelName),
        msg.receiverName, "",
        std::to_string(chanSlot ? chanSlot->zoneChannelId : 0u),
        std::to_string(chanSlot ? chanSlot->number : 0),
        chanSlot ? chanSlot->name : std::string(),
        std::to_string(chanSlot ? chanSlot->instanceId : 0u), "0", guidBuf
    });
}

void ChatHandler::addLocalChatLine(ChatType type, const std::string& message) {
    if (message.empty()) return;
    MessageChatData msg;
    msg.type = type;
    msg.language = ChatLanguage::UNIVERSAL;
    msg.message = message;
    addLocalChatMessage(msg);
}

void ChatHandler::addSystemChatMessage(const std::string& message) {
    addLocalChatLine(ChatType::SYSTEM, message);
}

// The CMSG_MESSAGECHAT that tells the server the player is away or busy, with
// the reply it is to give (0x006dc640, 0x006d52d0): the type, language 0 and
// the text, an empty one to clear it. The server sets the flag and answers
// whispers with CHAT_MSG_AFK / CHAT_MSG_DND itself. This client used to keep
// the state to itself and whisper English replies back on its own.
static void sendAwayState(GameHandler& owner, ChatType type, const std::string& text) {
    if (owner.getState() != WorldState::IN_WORLD || !owner.getSocket()) return;
    owner.getSocket()->send(MessageChatPacket::build(type, ChatLanguage::UNIVERSAL, text, ""));
}

void ChatHandler::toggleAfk(const std::string& message) {
    // /afk as 0x0050d170 takes it. Not away: away with the text or
    // DEFAULT_AFK_MESSAGE, saying MARKED_AFK_MESSAGE - and CLEARED_DND first
    // when busy (0x006dc640). Away with a text: only the new reply, nothing
    // said. Away without one: CLEARED_AFK (0x006d52d0). This printed English
    // and flipped the state whatever the text was.
    if (!owner_.afkStatusRef()) {
        const std::string text = message.empty()
            ? interfaceString("DEFAULT_AFK_MESSAGE", "Away from Keyboard") : message;
        if (owner_.dndStatusRef()) {
            addSystemChatMessage(interfaceString("CLEARED_DND", "You are no longer marked DND."));
            owner_.dndStatusRef() = false;
            owner_.dndMessageRef().clear();
        }
        addSystemChatMessage(interfaceFormat("MARKED_AFK_MESSAGE", "You are now AFK: %s", {text}));
        owner_.afkStatusRef() = true;
        owner_.afkMessageRef() = text;
        sendAwayState(owner_, ChatType::AFK, text);
    } else if (!message.empty()) {
        owner_.afkMessageRef() = message;
        sendAwayState(owner_, ChatType::AFK, message);
    } else {
        addSystemChatMessage(interfaceString("CLEARED_AFK", "You are no longer AFK."));
        owner_.afkStatusRef() = false;
        owner_.afkMessageRef().clear();
        afkAutoRepliedSenders_.clear();
        sendAwayState(owner_, ChatType::AFK, "");
    }

    LOG_INFO("AFK status: ", owner_.afkStatusRef(), ", message: ", message);
}

void ChatHandler::toggleDnd(const std::string& message) {
    // /dnd as 0x0050d170 takes it: away is cleared first where autoClearAFK
    // says so, with CLEARED_AFK, as for any other chat (0x006d52d0). Not busy:
    // busy with the text or DEFAULT_DND_MESSAGE, saying MARKED_DND. Busy with a
    // text: MARKED_DND with the new one. Busy without one: CLEARED_DND.
    if (owner_.afkStatusRef() && addons::storedCVarValue("autoClearAFK", "1") != "0") {
        toggleAfk("");
    }
    std::string text = message;
    if (!owner_.dndStatusRef()) {
        if (text.empty()) text = interfaceString("DEFAULT_DND_MESSAGE", "Do not Disturb");
        addSystemChatMessage(interfaceFormat("MARKED_DND", "You are now DND: %s.", {text}));
        owner_.dndStatusRef() = true;
        owner_.dndMessageRef() = text;
        // The server takes away off as it sets busy.
        owner_.afkStatusRef() = false;
        owner_.afkMessageRef().clear();
    } else if (text.empty()) {
        addSystemChatMessage(interfaceString("CLEARED_DND", "You are no longer marked DND."));
        owner_.dndStatusRef() = false;
        owner_.dndMessageRef().clear();
        afkAutoRepliedSenders_.clear();
    } else {
        addSystemChatMessage(interfaceFormat("MARKED_DND", "You are now DND: %s.", {text}));
        owner_.dndMessageRef() = text;
    }
    sendAwayState(owner_, ChatType::DND, text);

    LOG_INFO("DND status: ", owner_.dndStatusRef(), ", message: ", message);
}

void ChatHandler::replyToLastWhisper(const std::string& message) {
    if (!owner_.isInWorld()) {
        LOG_WARNING("Cannot send whisper: not in world or not connected");
        return;
    }

    // Nothing said for no one to answer or nothing to send: chatframe.lua's
    // REPLY leaves its "error message" branch empty and the client sends no
    // empty line. These printed "No one has whispered you yet." and raised
    // "You must specify a message to send." in English.
    if (owner_.lastWhisperSenderRef().empty() || message.empty()) return;

    // Send whisper using the standard message chat function
    sendChatMessage(ChatType::WHISPER, message, owner_.lastWhisperSenderRef());
    LOG_INFO("Replied to ", owner_.lastWhisperSenderRef(), ": ", message);
}

// ============================================================
// Moved opcode handlers (from GameHandler::registerOpcodeHandlers)
// ============================================================

namespace {
/// Order the panel's member list is drawn in. 0x00503990 ends its roster fill
/// with `_qsort(rows, count, 0x18, 0x004fb0f0)` - before the count and roster
/// events are raised - and that comparator never looks at a row's position: it
/// resolves each row's guid to the client's player record and compares the
/// *names*, answering that a row with no record goes after every named one
/// rather than among them. So the list is alphabetical with the unresolved at
/// the bottom, not in the order the server sent.
///
/// The client's compare is the locale one at 0x0076ec80, which folds case and
/// collates a German name the way the client's own list shows it; only ASCII is
/// folded here, which sorts an umlaut elsewhere. Stable, so members whose names
/// do not sort apart stay in the order the roster arrived.
///
/// Holding the unresolved rows back is the half that is not here: the client
/// counts them (DAT_00b743d8), raises no count or roster event while the count
/// stands, and asks the name store for each guid - when the last answer comes
/// back its callback (0x004ffe30) sorts and raises both then. Nothing here is
/// waiting on a name that has not arrived, so both events go out either way.
void sortChannelRoster(std::vector<ChannelMember>& roster) {
    const auto folded = [](const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (char c : s)
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        return out;
    };
    std::stable_sort(roster.begin(), roster.end(),
                     [&](const ChannelMember& a, const ChannelMember& b) {
        if (a.nameKnown != b.nameKnown) return a.nameKnown;
        return folded(a.name) < folded(b.name);
    });
}
} // namespace

/// The channel's own flag byte, which three packet bodies carry and all three
/// write the same way (0x00503990 with the roster, 0x00500380 beside a
/// member's own flags, and 0x00504130/0x005042f0 doing the same compare and
/// write): it
/// lands on the slot's +0x90, the very byte YOU_JOINED left, and the channel
/// is looked up in the display list *first* - a channel that is not listed
/// keeps the byte it had rather than quietly absorbing the new one, so the
/// next answer still knows it moved once it is listed. Only then, and only
/// when the byte actually changed, is CHANNEL_FLAGS_UPDATED (552) raised, with
/// nothing but that display index.
///
/// The panel listens for it (channelframe.lua:13) and answers by re-showing the
/// channel's dropdown, but only for the channel whose menu is open (:25,
/// `arg1 == ChannelListDropDown.clicked`) - which is how a player sees that the
/// channel they right-clicked turned moderated, passworded or voiced while that
/// menu sat open.
///
/// The client raises it a second time when a roster fill reaches the row that
/// is the player's own, because the voice bits it folded into that row (0x20,
/// 0x40, 0x80) come from its own voice session rather than from the wire and
/// may have moved a moment ago. Nothing here keeps voice state, so that row
/// would answer exactly as it did the first time; the second fire is not here.
void ChatHandler::noteChannelFlags(const std::string& chanName, uint8_t chanFlags) {
    if (chanName.empty()) return;
    chat_channels::Slot* slot = channelSlots_.byName(chanName);
    if (slot == nullptr) return;
    const int idx = getChannelIndex(chanName);
    if (idx <= 0 || slot->flags == chanFlags) return;
    slot->flags = chanFlags;
    owner_.fireAddonEvent("CHANNEL_FLAGS_UPDATED", {std::to_string(idx)});
}

void ChatHandler::handleChannelList(network::Packet& packet) {
    // A channel type byte comes first - Channel::List writes `uint8(1)` before
    // the name - and this read the name straight from it. The stray byte does
    // not shift anything, because the name's terminator ends the string either
    // way, but it rides along on the front of every use of it: the roster was
    // filed under "\x01General" while getChannelRoster is asked for "General",
    // so the member count answered zero for every channel.
    //
    // Recognised rather than assumed, since not every realm's build sends it: a
    // channel name always begins with a printable character, so a control byte
    // here is the type and nothing else can be.
    if (!packet.hasRemaining(1)) return;
    // The byte is whether the list is for the channel panel (0x00505dc0):
    // non-zero fills the roster and prints nothing, zero - /chatwho - prints
    // the members as CHAT_MSG_CHANNEL_LIST lines. Without the byte the client
    // would read the name's first letter as it, which is never zero.
    bool forChat = false;
    if (!packet.getData().empty() &&
        packet.getReadPos() < packet.getData().size() &&
        packet.getData()[packet.getReadPos()] < 0x20) {
        forChat = packet.readUInt8() == 0;
    }
    std::string chanName = packet.readString();
    if (!packet.hasRemaining(5)) return;
    const uint8_t chanFlags = packet.readUInt8();
    uint32_t memberCount = packet.readUInt32();
    memberCount = std::min(memberCount, 200u);

    noteChannelFlags(chanName, chanFlags);   // before a single member is read

    // The server's own member flags. What was here read 0x01 as moderator and
    // 0x02 as muted, so the channel's owner was announced as its moderator and
    // its moderators as muted.
    constexpr uint8_t kOwner = 0x01, kModerator = 0x02, kMuted = 0x08;

    std::vector<ChannelMember> roster;
    roster.reserve(memberCount);
    // The chat lines as 0x005029c0 builds them: each member "*" owner, "@"
    // moderator, nothing when voiced and "#" otherwise, then the name; joined
    // by ", " into lines under 256 bytes, each a CHAT_MSG_CHANNEL_LIST of the
    // channel, which chatframe.lua heads with CHAT_CHANNEL_LIST_GET. No count
    // line: this printed "X has N member(s):" and a line per member with
    // "[Owner]", "[Moderator]" and "[Muted]" after it, in English.
    std::string listLine;
    auto flushListLine = [&]() {
        if (listLine.empty()) return;
        MessageChatData line;
        line.type = ChatType::CHANNEL_LIST;
        line.language = ChatLanguage::UNIVERSAL;
        line.message = listLine;
        line.channelName = chanName;
        addLocalChatMessage(line);
        listLine.clear();
    };
    forChat = forChat && channelSlots_.byName(chanName) != nullptr;
    for (uint32_t i = 0; i < memberCount; ++i) {
        if (!packet.hasRemaining(9)) break;
        ChannelMember m;
        m.guid = packet.readUInt64();
        const uint8_t memberFlags = packet.readUInt8();
        m.owner     = (memberFlags & kOwner) != 0;
        m.moderator = (memberFlags & kModerator) != 0;
        m.muted     = (memberFlags & kMuted) != 0;

        auto entity = owner_.getEntityManager().getEntity(m.guid);
        if (entity) {
            auto player = std::dynamic_pointer_cast<Player>(entity);
            if (player && !player->getName().empty()) m.name = player->getName();
        }
        if (m.name.empty()) m.name = owner_.lookupName(m.guid);
        if (m.name.empty()) { m.name = "(unknown)"; m.nameKnown = false; }

        if (forChat) {
            const char* mark = (memberFlags & 0x01) ? "*" : (memberFlags & 0x02) ? "@"
                             : (memberFlags & 0x04) ? "" : "#";
            // "%s%s%s" into a 0x33-byte buffer: 50 bytes at most.
            std::string entry = (listLine.empty() ? std::string() : std::string(", ")) + mark + m.name;
            if (entry.size() > 50) entry.resize(50);
            if (entry.size() + listLine.size() >= 0x100) {
                flushListLine();
                if (entry.rfind(", ", 0) == 0) entry.erase(0, 2);
            }
            listLine += entry;
        }
        roster.push_back(std::move(m));
    }
    flushListLine();
    // Sorted after the chat lines were written, which is where the client's
    // sort sits too: /chatwho prints the server's order, the panel draws the
    // sorted one.
    sortChannelRoster(roster);

    channelRosters_[chanName] = std::move(roster);
    // Both, because the panel redraws its member list on the first and its
    // member count on the second, and it registers for each separately.
    //
    // By display index, not by name. ChannelRoster_Update and
    // ChannelList_CountUpdate both take a position in the channel list -
    // _G["ChannelButton"..id] is how the second finds its row, and the first is
    // called elsewhere with GetSelectedDisplayChannel(), which is an index. A
    // name built "ChannelButtonGeneral", which is nothing, so neither the
    // roster nor the count ever redrew.
    const int displayIndex = getChannelIndex(chanName);
    if (displayIndex > 0) {
        const std::string idx = std::to_string(displayIndex);
        const std::string count = std::to_string(channelRosters_[chanName].size());
        // Both carry the count. The roster one is 0x004ffe30's
        // event(548, "%d%d", displayIndex, members) - ChannelRoster_Update reads
        // only the first of the two, but the client passes both from the same
        // registers it has just stored at (&DAT_00b74464)[index*8].
        owner_.fireAddonEvent("CHANNEL_ROSTER_UPDATE", {idx, count});
        owner_.fireAddonEvent("CHANNEL_COUNT_UPDATE", {idx, count});
    }
}


/// SMSG_USERLIST_ADD / _REMOVE / _UPDATE - one member of a channel changing.
///
/// The full roster comes on SMSG_CHANNEL_LIST and is asked for; these arrive
/// unasked as people join, leave and are promoted, and without them the roster
/// is only ever as fresh as the last time somebody opened the list.
///
/// Two readers rather than one with a branch, because the three layouts are
/// not quite the same - remove carries no player flags - and a handler that
/// reads its fields under an `if` cannot be checked against the server by
/// tools/packet_layout_check.py, which reads the run of widths a body asks
/// for. It flagged the first version of this, correctly by its own lights.
///
///   ADD, UPDATE  guid(8) playerFlags(1) channelFlags(1) count(4) name
///   REMOVE       guid(8)                channelFlags(1) count(4) name
///
/// Only the add lists anyone. The update (0x00500380) and the remove
/// (0x005042f0) both walk the rows looking for the guid and `return` out of the
/// whole handler when it is not among them - no row made, no count, no roster
/// event - so a promotion, a mute or a leaving for a member this client never
/// listed is simply dropped. The add (0x00504130) is the one that appends, and
/// it appends before it does anything else.
void ChatHandler::applyUserlistChange(const std::string& chanName, uint64_t guid,
                                      uint8_t memberFlags, UserlistChange change) {
    if (chanName.empty()) return;
    // The same bits the full list reads, and the same trap: 0x01 is the owner
    // and 0x02 the moderator, not the other way round.
    constexpr uint8_t kOwner = 0x01, kModerator = 0x02, kMuted = 0x08;
    auto& roster = channelRosters_[chanName];
    auto it = std::find_if(roster.begin(), roster.end(),
                           [&](const ChannelMember& m) { return m.guid == guid; });

    if (change == UserlistChange::remove) {
        // 0x005042f0 walks the rows and returns out of the handler when the guid
        // is not there, so a leaving nobody listed says nothing at all - not
        // even the two events below, which the client raises only past the scan.
        if (it == roster.end()) return;
        roster.erase(it);
    } else {
        if (it == roster.end()) {
            // The update (0x00500380) scans the same way and bails: it changes a
            // member, it does not announce one. Only the add gets this far.
            if (change != UserlistChange::add) return;
            ChannelMember m;
            m.guid = guid;
            m.name = owner_.lookupName(guid);
            if (m.name.empty()) { m.name = "(unknown)"; m.nameKnown = false; }
            roster.push_back(std::move(m));
            it = roster.end() - 1;
        }
        it->owner     = (memberFlags & kOwner) != 0;
        it->moderator = (memberFlags & kModerator) != 0;
        it->muted     = (memberFlags & kMuted) != 0;
    }

    // One order for the panel's rows: a joiner lands at the end of the wire's
    // list while the rows around it are the sorted ones from the last full
    // roster, and the panel asks for a member by position.
    sortChannelRoster(roster);

    // By display index, for the reason spelled out in handleChannelList: both
    // of these take a position in the channel list, not a name.
    const int displayIndex = getChannelIndex(chanName);
    if (displayIndex > 0) {
        const std::string idx = std::to_string(displayIndex);
        const std::string count = std::to_string(roster.size());
        owner_.fireAddonEvent("CHANNEL_ROSTER_UPDATE", {idx, count});
        owner_.fireAddonEvent("CHANNEL_COUNT_UPDATE", {idx, count});
    }
}

void ChatHandler::handleUserlistAdd(network::Packet& packet, UserlistChange change) {
    if (!packet.hasRemaining(14)) return;
    const uint64_t guid = packet.readUInt64();
    const uint8_t memberFlags = packet.readUInt8();
    const uint8_t chanFlags = packet.readUInt8();  // the channel's own flags
    packet.readUInt32();   // how many are in it now
    const std::string chanName = packet.readString();
    // The channel's byte goes onto the slot before the member row is touched,
    // which is the order 0x00500380 reads them in - and it is what a menu that
    // is already open is waiting to hear about.
    noteChannelFlags(chanName, chanFlags);
    applyUserlistChange(chanName, guid, memberFlags, change);
}

void ChatHandler::handleUserlistRemove(network::Packet& packet) {
    if (!packet.hasRemaining(13)) return;
    const uint64_t guid = packet.readUInt64();
    const uint8_t chanFlags = packet.readUInt8();  // the channel's own flags
    packet.readUInt32();   // how many are in it now
    const std::string chanName = packet.readString();
    noteChannelFlags(chanName, chanFlags);         // same order as the add
    applyUserlistChange(chanName, guid, 0, UserlistChange::remove);
}

// ============================================================
// Methods moved from GameHandler
// ============================================================

void ChatHandler::updateGmTicket(const std::string& text) {
    if (!owner_.isInWorld() || !owner_.getSocket()) return;
    // CMSG_GMTICKET_UPDATETEXT (WotLK 3.3.5a): the new text and nothing else.
    // Creating carries the player's position because a new ticket records where
    // it was raised; editing one does not move it.
    network::Packet pkt(wireOpcode(Opcode::CMSG_GMTICKET_UPDATETEXT));
    pkt.writeString(text);
    owner_.getSocket()->send(pkt);
    LOG_INFO("Updated GM ticket text");
}

void ChatHandler::submitGmTicket(const std::string& text) {
    if (!owner_.isInWorld()) return;

    // CMSG_GMTICKET_CREATE:
    //   uint32  mapId
    //   float   x, y, z          (server coords, no facing)
    //   string  message
    //   uint32  needResponse
    //   uint8   needMoreHelp
    //   uint32  count            - how many chat-log timestamps follow
    //   uint32  decompressedSize - the zlib'd chat log after it, if any
    //
    // The comment above these writes described a layout in the other order,
    // opening with the text and carrying a facing the server does not read,
    // and the writes matched the comment rather than the wire. So the map id
    // was taken out of the middle of the ticket text, the position out of
    // whatever followed it, and the message read from four bytes of a float -
    // and the trailing fields were missing entirely, which runs the read off
    // the end of the buffer. AzerothCore catches that, logs, and drops the
    // packet: the ticket was never created and nothing said so.
    //
    // One shape on every expansion, and it is the long one.
    //
    // The prefix through the message is what all three read: a 1.12 and a
    // 2.4.3 server take mapId, x, y, z and the message and stop there. The
    // four fields after it are 3.x, and the asymmetry is what decides this -
    // a WotLK server that does not get them reads off the end of the buffer
    // and drops the whole request, while a vanilla or TBC one simply leaves
    // them unread. Trailing bytes nobody reads cost nothing; missing ones
    // cost the ticket. So there is nothing to gate on the expansion here, and
    // a gate would be a second thing to keep right.
    //
    // The chat log is a client-side transcript this client does not keep, so
    // both of its lengths go across as zero, which is how the real client
    // sends a ticket raised outside a conversation.
    //
    // The place is the player's on the wire (0x005ac8f0): the canonical one
    // went out as it was, X and Y the wrong way round.
    const auto& me = owner_.movementInfoRef();
    owner_.getSocket()->send(GmTicketCreatePacket::build(owner_.currentMapIdRef(), me.x, me.y, me.z, text));
    LOG_INFO("Submitted GM ticket: '", text, "'");
}

void ChatHandler::handleMotd(network::Packet& packet) {
    LOG_INFO("Handling SMSG_MOTD");

    MotdData data;
    if (!MotdParser::parse(packet, data)) {
        LOG_WARNING("Failed to parse SMSG_MOTD");
        return;
    }

    if (!data.isEmpty()) {
        LOG_INFO("========================================");
        LOG_INFO("   MESSAGE OF THE DAY");
        LOG_INFO("========================================");
        // Each line as it came, a CHAT_MSG_SYSTEM line of its own (0x00551660).
        // This put "MOTD: " in front and an empty line after the block.
        for (const auto& line : data.lines) {
            LOG_INFO(line);
            addSystemChatMessage(line);
        }
        LOG_INFO("========================================");
    }
}

} // namespace game
} // namespace wowee
