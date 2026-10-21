#include "game/gossip_quest_status.hpp"
#include <chrono>
#include "ui/framexml_takeover.hpp"
#include "game/item_text.hpp"
#include "game/quest_handler.hpp"
#include "game/quest_query_layout.hpp"
#include "game/game_handler.hpp"
#include "game/game_utils.hpp"
#include "game/entity.hpp"
#include "game/update_field_table.hpp"
#include "game/quest_progress.hpp"
#include "game/quest_text.hpp"
#include "game/packet_parsers.hpp"
#include "network/world_socket.hpp"
#include "network/wire_format.hpp"
#include "rendering/renderer.hpp"
#include "audio/audio_coordinator.hpp"
#include "audio/ui_sound_manager.hpp"
#include "pipeline/asset_manager.hpp"
#include "core/application.hpp"
#include "core/logger.hpp"
#include "game/interface_text.hpp"
#include <algorithm>
#include <bit>
#include <cstring>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <limits>
#include <set>
#include <sstream>
#include <unordered_map>

namespace wowee {
namespace game {

namespace {

/// A quest's EndText - the fourth string of SMSG_QUEST_QUERY_RESPONSE, which
/// the original keeps in its quest cache at +0x1a24 (where 0x007f70e0 reads it
/// in) - by quest id. It is what 0x006d8030 names an objective by when
/// SMSG_QUESTUPDATE_COMPLETE finishes it. Kept here because QuestLogEntry has
/// no field for it: completionText is a choice between it and the fifth.
std::unordered_map<uint32_t, std::string>& questEndTexts() {
    static std::unordered_map<uint32_t, std::string> texts;
    return texts;
}

/// The on-screen line for one kill or use credit, as 0x006d8030 words it for
/// SMSG_QUESTUPDATE_ADD_KILL: a game object (the entry's high bit) by its name
/// in ERR_QUEST_ADD_FOUND_SII, a creature by its name in ERR_QUEST_ADD_KILL_SII.
/// Empty when the name is not cached - the original prints nothing then.
///
/// The original first prefers the quest's own ObjectiveText for that entry
/// (cache +0x1c94), in ERR_QUEST_ADD_FOUND_SII; this client does not keep
/// those four strings, so such a quest shows the creature's name instead.
std::string questKillCreditText(const GameHandler& owner, uint32_t rawEntry,
                                uint32_t count, uint32_t required) {
    const uint32_t entry = rawEntry & 0x7FFFFFFFu;
    if ((rawEntry & 0x80000000u) != 0) {
        const auto* info = owner.getCachedGameObjectInfo(entry);
        if (!info || info->name.empty()) return {};
        return interfaceFormat("ERR_QUEST_ADD_FOUND_SII", "%1$s: %2$d/%3$d",
                               {info->name, std::to_string(count), std::to_string(required)});
    }
    const std::string name = owner.getCachedCreatureName(entry);
    if (name.empty()) return {};
    return interfaceFormat("ERR_QUEST_ADD_KILL_SII", "%1$s slain: %2$d/%3$d",
                           {name, std::to_string(count), std::to_string(required)});
}

/// The on-screen line SMSG_QUESTUPDATE_COMPLETE gives (0x006d8030, case
/// 0x198): the quest's EndText in ERR_QUEST_OBJECTIVE_COMPLETE_S, or
/// ERR_QUEST_UNKNOWN_COMPLETE where it has none.
std::string questObjectiveCompleteText(uint32_t questId) {
    const auto& texts = questEndTexts();
    auto it = texts.find(questId);
    if (it != texts.end() && !it->second.empty()) {
        return interfaceFormat("ERR_QUEST_OBJECTIVE_COMPLETE_S", "%s (Complete)", {it->second});
    }
    return interfaceString("ERR_QUEST_UNKNOWN_COMPLETE", "Objective Complete.");
}

/// A quest's title in one of the ERR_QUEST_* chat lines that name it.
std::string questTitleText(const char* key, const char* enUS, const std::string& title) {
    return interfaceFormat(key, enUS, {title});
}

}  // namespace

QuestGiverStatus QuestHandler::getQuestGiverStatus(uint64_t guid) const {
    auto it = npcQuestStatus_.find(guid);
    return (it != npcQuestStatus_.end()) ? it->second : QuestGiverStatus::NONE;
}

void QuestHandler::reconcileItemObjectivesFromInventory(
    const std::unordered_map<uint32_t, uint32_t>& carriedCounts) {
    bool changedAny = false;
    bool maybeCompletedObjective = false;
    // Which quests moved, so QUEST_WATCH_UPDATE can name each of them. The
    // event carries one log index and the interface auto-watches that quest;
    // firing it once with nothing named watched nothing at all.
    std::vector<uint32_t> movedQuests;
    for (auto& quest : questLog_) {
        if (quest.complete) continue;
        for (const auto& obj : quest.itemObjectives) {
            if (obj.itemId == 0 || obj.required == 0) continue;
            // Keep the derived required-count map in sync.
            quest.requiredItemCounts[obj.itemId] = obj.required;

            auto cit = carriedCounts.find(obj.itemId);
            const uint32_t held = (cit != carriedCounts.end()) ? cit->second : 0;
            const uint32_t newCount = std::min(held, obj.required);
            uint32_t& tracked = quest.itemCounts[obj.itemId];
            if (tracked == newCount) continue;

            const bool wasComplete = tracked >= obj.required;
            tracked = newCount;
            changedAny = true;
            if (std::find(movedQuests.begin(), movedQuests.end(), quest.questId)
                    == movedQuests.end()) {
                movedQuests.push_back(quest.questId);
            }
            if (!wasComplete && newCount >= obj.required)
                maybeCompletedObjective = true;

            // Push the per-objective progress to the on-screen tracker.
            std::string itemLabel = "item #" + std::to_string(obj.itemId);
            if (const ItemQueryResponseData* info = owner_.getItemInfo(obj.itemId)) {
                if (!info->name.empty()) itemLabel = info->name;
            } else {
                // Name not cached yet - request it so the next refresh labels
                // the objective properly.
                owner_.queryItemInfo(obj.itemId, 0);
            }
            if (owner_.questProgressCallbackRef())
                owner_.questProgressCallbackRef()(quest.title, itemLabel, newCount, obj.required);
        }
    }

    if (changedAny && owner_.addonEventCallbackRef()) {
        for (uint32_t moved : movedQuests) {
            const int index = questLogIndexOf(moved);
            if (index > 0) {
                owner_.addonEventCallbackRef()("QUEST_WATCH_UPDATE",
                                               {std::to_string(index)});
            }
        }
        owner_.addonEventCallbackRef()("QUEST_LOG_UPDATE", {});
        owner_.addonEventCallbackRef()("UNIT_QUEST_LOG_CHANGED", {"player"});
    }
    // Collecting the last item may have made a quest turn-in-able (! → ?),
    // so refresh nearby giver markers immediately.
    if (maybeCompletedObjective) requeryNearbyQuestGiverStatus();
}

/// SMSG_ITEM_PUSH_RESULT's quest line (0x006d7de0 -> 0x006d7c30): the first
/// quest in the log asking for this item, while the count before the push was
/// short of the requirement, shows ERR_QUEST_ADD_ITEM_SII with the new count
/// held to it. The client registers no SMSG_QUESTUPDATE_ADD_ITEM handler; this
/// is where the line comes from.
void QuestHandler::announceQuestItemPushed(uint32_t itemId, uint32_t count,
                                           uint32_t totalCount, const std::string& itemName) {
    for (const auto& quest : questLog_) {
        for (const auto& obj : quest.itemObjectives) {
            if (obj.itemId != itemId || obj.required == 0) continue;
            const int64_t before = int64_t(totalCount) - int64_t(count);
            if (before < int64_t(obj.required) && !itemName.empty()) {
                owner_.addUIInfoMessage(interfaceFormat("ERR_QUEST_ADD_ITEM_SII", "%1$s: %2$d/%3$d",
                    {itemName, std::to_string(std::min(totalCount, obj.required)),
                     std::to_string(obj.required)}));
            }
            return;
        }
    }
}

/// Say that a quest moved, to everything that draws one.
///
/// The quest log this client keeps and the quest log the interface draws are
/// two different things, and only an event joins them. Marking a quest
/// complete and telling nobody is why an objective the server credited -
/// searching the Altar of Zul, and every other explore objective - appeared
/// only after a relog: the state was right the whole time, and nothing had
/// asked the tracker to look at it again.
///
/// The same three every other quest-progress path fires, by the same rules:
/// the watch update carries the quest's position in the log, because that is
/// what the interface auto-watches from.
void QuestHandler::announceQuestLogChanged(uint32_t questId) {
    if (!owner_.addonEventCallbackRef()) return;
    const int index = questLogIndexOf(questId);
    if (index > 0) {
        owner_.addonEventCallbackRef()("QUEST_WATCH_UPDATE", {std::to_string(index)});
    }
    owner_.addonEventCallbackRef()("QUEST_LOG_UPDATE", {});
    owner_.addonEventCallbackRef()("UNIT_QUEST_LOG_CHANGED", {"player"});
}

void QuestHandler::requeryNearbyQuestGiverStatus() {
    if (questGiverRequeryCooldown_ > 0.0f) {
        questGiverRequeryPending_ = true;
        return;
    }
    sendQuestGiverStatusQueries();
    questGiverRequeryCooldown_ = kQuestGiverRequeryIntervalSec;
}

void QuestHandler::tickQuestGiverStatusRequery(float deltaTime) {
    if (questGiverRequeryCooldown_ <= 0.0f) return;
    questGiverRequeryCooldown_ -= deltaTime;
    if (questGiverRequeryCooldown_ > 0.0f) return;

    questGiverRequeryCooldown_ = 0.0f;
    if (questGiverRequeryPending_) {
        questGiverRequeryPending_ = false;
        sendQuestGiverStatusQueries();
        questGiverRequeryCooldown_ = kQuestGiverRequeryIntervalSec;
    }
}

void QuestHandler::sendQuestGiverStatusQueries() {
    if (!owner_.getSocket()) return;
    for (const auto& [guid, entity] : owner_.getEntityManager().getEntities()) {
        if (entity->getType() != ObjectType::UNIT) continue;
        auto unit = std::static_pointer_cast<Unit>(entity);
        if (unit->getNpcFlags() & NPC_FLAG_QUESTGIVER) {
            network::Packet qsPkt(wireOpcode(Opcode::CMSG_QUESTGIVER_STATUS_QUERY));
            qsPkt.writeUInt64(guid);
            owner_.getSocket()->send(qsPkt);
        }
    }
}


// The readability test and the completed-line choice live in game/quest_text.hpp
// so they can be tested without a packet; this is the name the file has always
// called the first of them by.
static bool isReadableQuestText(const std::string& s, size_t minLen, size_t maxLen) {
    return questTextIsReadable(s, minLen, maxLen);
}

static bool isPlaceholderQuestTitle(const std::string& s) {
    return s.rfind("Quest #", 0) == 0;
}

static bool looksLikeQuestDescriptionText(const std::string& s) {
    int spaces = 0;
    int commas = 0;
    for (unsigned char c : s) {
        if (c == ' ') spaces++;
        if (c == ',') commas++;
    }
    const int words = spaces + 1;
    if (words > 8) return true;
    if (commas > 0 && words > 5) return true;
    if (s.find(". ") != std::string::npos) return true;
    if (s.find(':') != std::string::npos && words > 5) return true;
    return false;
}

static bool isStrongQuestTitle(const std::string& s) {
    if (!isReadableQuestText(s, 6, 72)) return false;
    if (looksLikeQuestDescriptionText(s)) return false;
    unsigned char first = static_cast<unsigned char>(s.front());
    return std::isupper(first) != 0;
}

static int scoreQuestTitle(const std::string& s) {
    if (!isReadableQuestText(s, 4, 72)) return -1000;
    if (looksLikeQuestDescriptionText(s)) return -1000;
    int score = 0;
    score += static_cast<int>(std::min<size_t>(s.size(), 32));
    unsigned char first = static_cast<unsigned char>(s.front());
    if (std::isupper(first)) score += 20;
    if (std::islower(first)) score -= 20;
    if (s.find(' ') != std::string::npos) score += 8;
    if (s.find('.') != std::string::npos) score -= 18;
    if (s.find('!') != std::string::npos || s.find('?') != std::string::npos) score -= 6;
    return score;
}

static bool readCStringAt(const std::vector<uint8_t>& data, size_t start, std::string& out, size_t& nextPos) {
    out.clear();
    if (start >= data.size()) return false;
    size_t i = start;
    while (i < data.size()) {
        uint8_t b = data[i++];
        if (b == 0) {
            nextPos = i;
            return true;
        }
        out.push_back(static_cast<char>(b));
    }
    return false;
}

struct QuestQueryTextCandidate {
    std::string title;
    std::string objectives;
    /// The third string: what the quest giver says, shown above the objectives
    /// in the quest log. It was being read and thrown away - the walk to the
    /// fifth string passes straight over it.
    std::string description;
    std::string completionText;
    /// The fourth string, EndText, which names an exploration or event
    /// objective once it is done (see questEndTexts).
    std::string endText;
    /// The raw third string as read from the wire, before the readability
    /// check - a temporary probe into why the description comes back empty on
    /// some realms even where the title and objectives read cleanly.
    std::string debugThirdRaw;
    int score = -1000;
};

// Expand WoW text tokens ($b line breaks etc.) in quest strings; titles are
// single-line so their line breaks collapse to spaces instead
static std::string normalizeQuestText(std::string s, bool singleLine) {
    s = normalizeWowTextTokens(std::move(s));
    if (singleLine) {
        std::string out;
        out.reserve(s.size());
        bool lastSpace = false;
        for (char ch : s) {
            if (ch == '\n' || ch == '\r' || ch == ' ') {
                if (!lastSpace && !out.empty()) out.push_back(' ');
                lastSpace = true;
            } else {
                out.push_back(ch);
                lastSpace = false;
            }
        }
        while (!out.empty() && out.back() == ' ') out.pop_back();
        s = std::move(out);
    }
    return s;
}

/// Whether the objective records really do begin where this string run ends.
///
/// The five strings are followed by the kill and item objectives, which are
/// numbers with a shape - an entry id and a count, both inside plausible
/// bounds. So a candidate offset can be checked rather than guessed at: read
/// past its strings and ask whether what follows parses as the block that must
/// follow them. Defined below, beside the parse it borrows.
static bool questStringRunFitsObjectives(const std::vector<uint8_t>& data,
                                         size_t stringsStart, int nStrings,
                                         bool wotlkLayout);

/// The completed line, chosen in game/quest_text.hpp and then expanded: the
/// $b and $n tokens in it are the server's, and every other quest string on
/// its way to the interface goes through the same normalisation.
static std::string pickQuestCompletedLine(const std::string& endText,
                                          const std::string& completedText) {
    std::string chosen = questCompletedText(endText, completedText);
    if (chosen.empty()) return {};
    return normalizeQuestText(std::move(chosen), false);
}

static QuestQueryTextCandidate pickBestQuestQueryTexts(const std::vector<uint8_t>& data, bool classicHint) {
    QuestQueryTextCandidate best;
    if (data.size() <= 9) return best;

    // Where the first string sits: straight after the numeric block. The
    // WotLK count is itemized in quest_query_layout.hpp and pinned by a test
    // that builds the block the way the server writes it.
    //
    // This said fifty-seven fields, which lands thirty-two bytes inside the
    // reward block. The read there is not a string, the readability check drops
    // it, and the scan below rescues most quests by finding the first printable
    // run - but the scan knows a title only by looking like one, so a quest
    // whose objectives read more like a title than its title does came out with
    // the objectives in the title and everything after it shifted by one.
    // Quests 1712 and 5250 in the log are both that.
    //
    // Classic's count is not verified the same way and is left alone; a seed
    // that reads as nothing still falls through to the scan, which is where
    // both of them were already being answered from.
    std::vector<size_t> seedOffsets;
    const size_t base = 8;
    const size_t classicOffset = base + 40u * 4u;
    const size_t wotlkOffset = kWotlkQuestQueryStringsOffset;
    if (classicHint) {
        seedOffsets.push_back(classicOffset);
        seedOffsets.push_back(wotlkOffset);
    } else {
        seedOffsets.push_back(wotlkOffset);
        seedOffsets.push_back(classicOffset);
    }
    for (size_t off : seedOffsets) {
        if (off < data.size()) {
            std::string title;
            size_t next = off;
            if (readCStringAt(data, off, title, next)) {
                QuestQueryTextCandidate c;
                c.title = normalizeQuestText(title, true);
                c.score = scoreQuestTitle(c.title) + 20; // Prefer expected struct offsets
                // And prefer, far above anything a scan can claim, an offset
                // whose strings are followed by a block that reads as the
                // objectives. That is not a preference between two guesses:
                // the numbers after the strings have a shape, and an offset
                // whose run ends exactly at one is the offset the server
                // wrote. Without this a suffix of some other string could
                // outscore a short title - "Sven's Revenge" is fourteen
                // characters - and win the whole candidate, completed line
                // included, which is one way that line came back empty on a
                // quest that had one.
                const bool wotlkSeed = (off == wotlkOffset);
                if (questStringRunFitsObjectives(data, off, wotlkSeed ? 5 : 4,
                                                 wotlkSeed)) {
                    c.score += 100;
                }

                std::string s2;
                size_t n2 = next;
                if (readCStringAt(data, next, s2, n2) && isReadableQuestText(s2, 8, 600)) {
                    c.objectives = normalizeQuestText(s2, false);
                }

                // The strings run Title, Objectives, Details, AreaDescription,
                // CompletedText - read off AzerothCore's Quest::BuildQuestData,
                // where they are written in that order after the numeric block.
                // The fifth is what the quest log and the world map's quest
                // list show once every objective is done, and answering it with
                // "" left a completed quest with a blank line where "Return to
                // Marshal Dughan" belongs.
                //
                // The strings after the title read Title, Objectives, Details,
                // (AreaDescription,) CompletedText in every expansion - the
                // order is the same for vmangos classic and AzerothCore WotLK,
                // which the quest-reward tests pin byte for byte. Only the
                // numeric block before them changes size, and the seed offsets
                // above already try both, with the score picking whichever read
                // cleanly. So the third string is the description whatever the
                // client is, and gating it to WotLK left classic and TBC quests
                // with a blank description panel - the one thing a universal
                // client cannot do.
                //
                // Still only from the seeded offsets, never the scored scan
                // below: that finds a title by printability alone and says
                // nothing about what follows it. And still guarded by
                // readability, so a wrong offset - a title matched at the other
                // expansion's seed - yields an unreadable third string and is
                // dropped rather than pasted onto the quest.
                {
                    std::string s3, s4, s5;
                    size_t n3 = n2, n4 = n2, n5 = n2;
                    if (readCStringAt(data, n2, s3, n3)) {
                        c.debugThirdRaw = s3;   // probe: what the third string held
                        // The details, above the objectives in the log.
                        if (isReadableQuestText(s3, 8, 4096)) {
                            c.description = normalizeQuestText(s3, false);
                        }
                        // The completed line sits one string later on WotLK
                        // (past EndText) than on the earlier clients, where
                        // EndText is the last string there is. Read both and
                        // let questCompletedText say which one this quest
                        // actually carries.
                        if (readCStringAt(data, n3, s4, n4)) {
                            readCStringAt(data, n4, s5, n5);
                            c.completionText = pickQuestCompletedLine(s4, s5);
                            if (isReadableQuestText(s4, 2, 600))
                                c.endText = normalizeQuestText(s4, true);
                        }
                    }
                }
                if (c.score > best.score) best = c;
            }
        }
    }

    // Fallback: scan packet for best printable C-string title candidate.
    for (size_t start = 8; start < data.size(); ++start) {
        std::string title;
        size_t next = start;
        if (!readCStringAt(data, start, title, next)) continue;

        QuestQueryTextCandidate c;
        c.title = normalizeQuestText(title, true);
        c.score = scoreQuestTitle(c.title);
        if (c.score < 0) continue;

        std::string s2, s3;
        size_t n2 = next, n3 = next;
        size_t afterObj = next;
        if (readCStringAt(data, next, s2, n2)) {
            if (isReadableQuestText(s2, 8, 600)) { c.objectives = normalizeQuestText(s2, false); afterObj = n2; }
            else if (readCStringAt(data, n2, s3, n3) && isReadableQuestText(s3, 8, 600)) { c.objectives = normalizeQuestText(s3, false); afterObj = n3; }
        }
        // The fallback found the title by its printable content, so whatever
        // follows is the same run the seeded path reads - Objectives, Details,
        // AreaDescription, CompletedText - only reached without knowing the size
        // of the numeric block ahead of it. AzerothCore's block is larger than
        // either seed above (its reward/reputation arrays push the strings past
        // the WotLK seed), so on a live 3.3.5 realm this scan is what wins, and
        // it used to stop at the objectives and leave every quest's description
        // panel blank while title and objectives came through fine - the exact
        // shape the logs showed. Read the description and completed line here
        // too, anchored to the end of the objectives string and guarded the
        // same way, so the winner carries a description whichever path wins.
        if (!c.objectives.empty()) {
            std::string d3, d4, d5;
            size_t m3 = afterObj, m4 = afterObj, m5 = afterObj;
            if (readCStringAt(data, afterObj, d3, m3)) {
                if (c.debugThirdRaw.empty()) c.debugThirdRaw = d3;
                if (isReadableQuestText(d3, 8, 4096)) c.description = normalizeQuestText(d3, false);
                if (readCStringAt(data, m3, d4, m4)) {
                    readCStringAt(data, m4, d5, m5);
                    c.completionText = pickQuestCompletedLine(d4, d5);
                    if (isReadableQuestText(d4, 2, 600))
                        c.endText = normalizeQuestText(d4, true);
                }
            }
        }
        if (c.score > best.score) best = c;
    }

    return best;
}

struct QuestQueryObjectives {
    struct Kill { int32_t npcOrGoId; uint32_t required; uint32_t sourceItem = 0; };
    struct Item { uint32_t itemId; uint32_t required; };
    std::array<Kill, 4> kills{};
    std::array<Item, 6> items{};
    bool valid = false;
};

// The objective records as SMSG_QUEST_QUERY_RESPONSE lays them out, every
// field a little-endian 32-bit word - so no padding, and a memcpy reads them
// on the little-endian hosts this builds for.
static_assert(std::endian::native == std::endian::little);

/// Wrath: one of the four creature/object records.
struct WotlkKillObjectiveRecord {
    uint32_t npcOrGoId;      ///< high bit set: a game object
    uint32_t required;
    uint32_t itemDrop;       ///< ItemDrop + its count: not inventory objectives
    uint32_t itemDropCount;
};
static_assert(sizeof(WotlkKillObjectiveRecord) == 16);

/// Wrath: one of the six required-item records, after the four above.
struct WotlkItemObjectiveRecord {
    uint32_t itemId;
    uint32_t required;
};
static_assert(sizeof(WotlkItemObjectiveRecord) == 8);

/// Classic/TBC: a creature/object and a required item, interleaved.
struct ClassicObjectiveRecord {
    uint32_t npcOrGoId;
    uint32_t killsRequired;
    uint32_t itemId;
    uint32_t itemsRequired;
};
static_assert(sizeof(ClassicObjectiveRecord) == 16);

/// The record at `pos`; the caller has checked that it fits.
template <class Record>
static Record objectiveRecordAt(const std::vector<uint8_t>& data, size_t pos) {
    Record record;
    std::memcpy(&record, &data[pos], sizeof(record));
    return record;
}

static int32_t decodeQuestNpcOrGo(uint32_t raw) {
    if ((raw & 0x80000000u) != 0)
        return -static_cast<int32_t>(raw & 0x7FFFFFFFu);
    return static_cast<int32_t>(raw);
}

static bool plausibleQuestObjective(int32_t id, uint32_t required) {
    const uint32_t magnitude = id < 0 ? static_cast<uint32_t>(-static_cast<int64_t>(id))
                                      : static_cast<uint32_t>(id);
    return magnitude <= 0x00FFFFFFu && required <= 0x0000FFFFu;
}

static QuestQueryObjectives tryParseQuestObjectivesAt(const std::vector<uint8_t>& data,
                                                       size_t startPos, int nStrings,
                                                       bool wotlkLayout) {
    QuestQueryObjectives out;
    size_t pos = startPos;

    for (int si = 0; si < nStrings; ++si) {
        while (pos < data.size() && data[pos] != 0) ++pos;
        if (pos >= data.size()) return out;
        ++pos;
    }

    if (wotlkLayout) {
        // Wrath: four {npc/go, count, sourceItem, sourceCount} records,
        // followed by six {requiredItem, count} records.
        for (int i = 0; i < 4; ++i) {
            if (pos + sizeof(WotlkKillObjectiveRecord) > data.size()) return out;
            const auto kill = objectiveRecordAt<WotlkKillObjectiveRecord>(data, pos);
            pos += sizeof(kill);
            out.kills[i].npcOrGoId = decodeQuestNpcOrGo(kill.npcOrGoId);
            out.kills[i].required  = kill.required;
            out.kills[i].sourceItem = kill.itemDrop;
            if (!plausibleQuestObjective(out.kills[i].npcOrGoId, out.kills[i].required))
                return {};
        }
        for (int i = 0; i < 6; ++i) {
            if (pos + sizeof(WotlkItemObjectiveRecord) > data.size()) return out;
            const auto item = objectiveRecordAt<WotlkItemObjectiveRecord>(data, pos);
            pos += sizeof(item);
            out.items[i].itemId   = item.itemId;
            out.items[i].required = item.required;
            if (!plausibleQuestObjective(static_cast<int32_t>(out.items[i].itemId),
                                         out.items[i].required))
                return {};
        }
    } else {
        // Classic/TBC: four interleaved {npc/go, count, requiredItem, count} records.
        for (int i = 0; i < 4; ++i) {
            if (pos + sizeof(ClassicObjectiveRecord) > data.size()) return out;
            const auto record = objectiveRecordAt<ClassicObjectiveRecord>(data, pos);
            pos += sizeof(record);
            out.kills[i].npcOrGoId = decodeQuestNpcOrGo(record.npcOrGoId);
            out.kills[i].required  = record.killsRequired;
            out.items[i].itemId    = record.itemId;
            out.items[i].required  = record.itemsRequired;
            if (!plausibleQuestObjective(out.kills[i].npcOrGoId, out.kills[i].required) ||
                !plausibleQuestObjective(static_cast<int32_t>(out.items[i].itemId),
                                         out.items[i].required))
                return {};
        }
    }

    out.valid = true;
    return out;
}

static bool questStringRunFitsObjectives(const std::vector<uint8_t>& data,
                                         size_t stringsStart, int nStrings,
                                         bool wotlkLayout) {
    return tryParseQuestObjectivesAt(data, stringsStart, nStrings, wotlkLayout).valid;
}

static QuestQueryObjectives extractQuestQueryObjectives(const std::vector<uint8_t>& data,
                                                         uint8_t questLogStride) {
    if (data.size() < 16) return {};

    const size_t base = 8;
    // Fixed-width fields between questMethod and the first localized string.
    // These counts come directly from each expansion's query response serializer.
    if (questLogStride >= 5)
        return tryParseQuestObjectivesAt(data, base + 63u * 4u, 5, true);  // WotLK
    if (questLogStride == 4)
        return tryParseQuestObjectivesAt(data, base + 41u * 4u, 4, false); // TBC
    return tryParseQuestObjectivesAt(data, base + 37u * 4u, 4, false);     // Classic/Turtle
}

// Quest log reward extraction lives in QuestQueryRewardsParser
// (world_packets_world.cpp) so the per-expansion offsets are unit-testable.

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

QuestHandler::QuestHandler(GameHandler& owner)
    : owner_(owner) {}

// ---------------------------------------------------------------------------
// Opcode registrations
// ---------------------------------------------------------------------------

void QuestHandler::registerOpcodes(DispatchTable& table) {

    // ---- SMSG_GOSSIP_MESSAGE ----
    table[Opcode::SMSG_GOSSIP_MESSAGE] = [this](network::Packet& packet) { handleGossipMessage(packet); };

    // ---- SMSG_QUESTGIVER_QUEST_LIST ----
    table[Opcode::SMSG_QUESTGIVER_QUEST_LIST] = [this](network::Packet& packet) { handleQuestgiverQuestList(packet); };

    // ---- SMSG_GOSSIP_COMPLETE ----
    table[Opcode::SMSG_GOSSIP_COMPLETE] = [this](network::Packet& packet) { handleGossipComplete(packet); };

    // ---- SMSG_NPC_TEXT_UPDATE ----
    table[Opcode::SMSG_NPC_TEXT_UPDATE] = [this](network::Packet& packet) { handleNpcTextUpdate(packet); };

    // ---- SMSG_GOSSIP_POI ----
    table[Opcode::SMSG_GOSSIP_POI] = [this](network::Packet& packet) {
        // The place comes as the wire has it; every reader of gossipPois_
        // takes it canonical, as the quest points beside it are.
        GossipPoiData read;
        if (!GossipPoiParser::parse(packet, read)) return;
        const float poiX = read.canonicalX;
        const float poiY = read.canonicalY;
        const uint32_t icon = read.icon;
        GossipPoi poi; poi.x = poiX; poi.y = poiY; poi.icon = icon; poi.data = read.importance;
        poi.name = std::move(read.name);
        poi.flags = read.flags;
        poi.receivedMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
        if (gossipPois_.size() >= 200) gossipPois_.erase(gossipPois_.begin());
        gossipPois_.push_back(std::move(poi));
        LOG_DEBUG("SMSG_GOSSIP_POI: x=", poiX, " y=", poiY, " icon=", icon);
    };

    // ---- SMSG_QUESTGIVER_QUEST_DETAILS ----
    table[Opcode::SMSG_QUESTGIVER_QUEST_DETAILS] = [this](network::Packet& packet) { handleQuestDetails(packet); };

    // ---- SMSG_QUESTLOG_FULL ----
    table[Opcode::SMSG_QUESTLOG_FULL] = [this](network::Packet& /*packet*/) {
        // ERR_QUEST_LOG_FULL on screen and nowhere else (0x006d7f10, case
        // 0x195). This was an English line of its own, in chat as well.
        owner_.addUIError(interfaceString("ERR_QUEST_LOG_FULL", "Your quest log is full."));
        // Roll back the optimistic local entries the server refused; without
        // this the local log drifts past the server cap with phantom quests.
        bool removed = false;
        for (const auto& [pendingQuestId, npcGuid] : pendingQuestAcceptNpcGuids_) {
            (void)npcGuid;
            if (findQuestLogSlotIndexFromServer(pendingQuestId) < 0) {
                removed |= std::erase_if(questLog_, [&](const QuestLogEntry& q) {
                    return q.questId == pendingQuestId;
                }) > 0;
            }
        }
        pendingQuestAcceptTimeouts_.clear();
        pendingQuestAcceptNpcGuids_.clear();
        if (removed && owner_.addonEventCallbackRef())
            owner_.addonEventCallbackRef()("QUEST_LOG_UPDATE", {});
    };

    // ---- SMSG_QUESTGIVER_REQUEST_ITEMS ----
    table[Opcode::SMSG_QUESTGIVER_REQUEST_ITEMS] = [this](network::Packet& packet) { handleQuestRequestItems(packet); };

    // ---- SMSG_QUESTGIVER_OFFER_REWARD ----
    table[Opcode::SMSG_QUESTGIVER_OFFER_REWARD] = [this](network::Packet& packet) { handleQuestOfferReward(packet); };

    // ---- SMSG_QUEST_CONFIRM_ACCEPT ----
    table[Opcode::SMSG_QUEST_CONFIRM_ACCEPT] = [this](network::Packet& packet) { handleQuestConfirmAccept(packet); };

    // ---- SMSG_QUEST_POI_QUERY_RESPONSE ----
    table[Opcode::SMSG_QUEST_POI_QUERY_RESPONSE] = [this](network::Packet& packet) { handleQuestPoiQueryResponse(packet); };

    // ---- SMSG_QUESTGIVER_STATUS ----
    table[Opcode::SMSG_QUESTGIVER_STATUS] = [this](network::Packet& packet) {
        if (packet.hasRemaining(9)) {
            uint64_t npcGuid = packet.readUInt64();
            uint8_t status = owner_.getPacketParsers()->readQuestGiverStatus(packet);
            LOG_INFO("SMSG_QUESTGIVER_STATUS: npcGuid=0x", std::hex, npcGuid, std::dec,
                     " status=", static_cast<int>(status));
            npcQuestStatus_[npcGuid] = static_cast<QuestGiverStatus>(status);
        }
    };

    // ---- SMSG_QUESTGIVER_STATUS_MULTIPLE ----
    table[Opcode::SMSG_QUESTGIVER_STATUS_MULTIPLE] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(4)) return;
        uint32_t count = packet.readUInt32();
        for (uint32_t i = 0; i < count; ++i) {
            if (!packet.hasRemaining(9)) break;
            uint64_t npcGuid = packet.readUInt64();
            uint8_t status = owner_.getPacketParsers()->readQuestGiverStatus(packet);
            npcQuestStatus_[npcGuid] = static_cast<QuestGiverStatus>(status);
        }
    };

    // ---- SMSG_QUESTUPDATE_FAILED ----
    table[Opcode::SMSG_QUESTUPDATE_FAILED] = [this](network::Packet& packet) {
        if (packet.hasRemaining(4)) {
            uint32_t questId = packet.readUInt32();
            std::string questTitle;
            for (const auto& q : questLog_)
                if (q.questId == questId && !q.title.empty()) { questTitle = q.title; break; }
            // ERR_QUEST_FAILED_S with the title, in chat (0x006d8030, case
            // 0x196); nothing for a quest whose title is not known. This was
            // English: '"X" failed!'.
            if (!questTitle.empty())
                owner_.addSystemChatMessage(questTitleText("ERR_QUEST_FAILED_S", "%s failed.", questTitle));
            // Same omission as the complete handler had: a line of chat is not
            // a redraw, and a failed quest goes on looking fine in the log.
            announceQuestLogChanged(questId);
        }
    };

    // ---- SMSG_QUESTUPDATE_FAILEDTIMER ----
    table[Opcode::SMSG_QUESTUPDATE_FAILEDTIMER] = [this](network::Packet& packet) {
        if (packet.hasRemaining(4)) {
            uint32_t questId = packet.readUInt32();
            std::string questTitle;
            for (const auto& q : questLog_)
                if (q.questId == questId && !q.title.empty()) { questTitle = q.title; break; }
            // The same ERR_QUEST_FAILED_S as a failed quest: case 0x197 of
            // 0x006d8030 stops the timer and falls into 0x196. This was
            // English: '"X" has timed out.'.
            if (!questTitle.empty())
                owner_.addSystemChatMessage(questTitleText("ERR_QUEST_FAILED_S", "%s failed.", questTitle));
            announceQuestLogChanged(questId);
        }
    };

    // ---- SMSG_QUESTGIVER_QUEST_FAILED ----
    table[Opcode::SMSG_QUESTGIVER_QUEST_FAILED] = [this](network::Packet& packet) {
        // uint32 questId + uint32 reason
        if (packet.hasRemaining(8)) {
            uint32_t questId = packet.readUInt32();
            uint32_t reason = packet.readUInt32();
            std::string questTitle;
            for (const auto& q : questLog_)
                if (q.questId == questId && !q.title.empty()) { questTitle = q.title; break; }
            // As 0x006cda70 words it, by the inventory result the server
            // gives as the reason: a full bag (4, 0x10, 0x32) is
            // ERR_QUEST_FAILED_BAG_FULL_S in chat and ERR_INV_FULL on screen,
            // a duplicate (0x11) ERR_QUEST_FAILED_MAX_COUNT_S, the daily cap
            // (0x4a) ERR_QUEST_FAILED_TOO_MANY_DAILY_QUESTS_I with 25, and
            // anything else ERR_QUEST_FAILED_S. Nothing for a quest whose
            // title is not known. This printed its own English reasons.
            if (!questTitle.empty()) {
                if (reason == 0x4a) {
                    owner_.addSystemChatMessage(interfaceFormat("ERR_QUEST_FAILED_TOO_MANY_DAILY_QUESTS_I",
                        "You have already completed %d daily quests today", {"25"}));
                } else if (reason == 4 || reason == 0x10 || reason == 0x32) {
                    owner_.addSystemChatMessage(questTitleText("ERR_QUEST_FAILED_BAG_FULL_S",
                                                               "%s failed: Inventory is full.", questTitle));
                    owner_.addUIError(interfaceString("ERR_INV_FULL", "Inventory is full."));
                } else if (reason == 0x11) {
                    owner_.addSystemChatMessage(questTitleText("ERR_QUEST_FAILED_MAX_COUNT_S",
                                                               "%s failed: Duplicate item found.", questTitle));
                } else {
                    owner_.addSystemChatMessage(questTitleText("ERR_QUEST_FAILED_S", "%s failed.", questTitle));
                }
            }
        }
    };

    // ---- SMSG_QUESTGIVER_QUEST_INVALID ----
    table[Opcode::SMSG_QUESTGIVER_QUEST_INVALID] = [this](network::Packet& packet) {
        if (packet.hasRemaining(4)) {
            uint32_t failReason = packet.readUInt32();
            pendingTurnInRewardRequest_ = false;
            const char* reasonStr = "Unknown";
            switch (failReason) {
                case 0: reasonStr = "Don't have quest"; break;
                case 1: reasonStr = "Quest level too low"; break;
                case 4: reasonStr = "Insufficient money"; break;
                case 5: reasonStr = "Inventory full"; break;
                case 13: reasonStr = "Already on that quest"; break;
                case 18: reasonStr = "Already completed quest"; break;
                case 19: reasonStr = "Can't take any more quests"; break;
            }
            LOG_WARNING("Quest invalid: reason=", failReason, " (", reasonStr, ")");
            if (!pendingQuestAcceptTimeouts_.empty()) {
                std::vector<uint32_t> pendingQuestIds;
                pendingQuestIds.reserve(pendingQuestAcceptTimeouts_.size());
                for (const auto& pending : pendingQuestAcceptTimeouts_) {
                    pendingQuestIds.push_back(pending.first);
                }
                for (uint32_t questId : pendingQuestIds) {
                    const uint64_t npcGuid = pendingQuestAcceptNpcGuids_.count(questId) != 0
                        ? pendingQuestAcceptNpcGuids_[questId] : 0;
                    if (failReason == 13) {
                        std::string fallbackTitle = "Quest #" + std::to_string(questId);
                        std::string fallbackObjectives;
                        if (currentQuestDetails_.questId == questId) {
                            if (!currentQuestDetails_.title.empty()) fallbackTitle = currentQuestDetails_.title;
                            fallbackObjectives = currentQuestDetails_.objectives;
                        }
                        addQuestToLocalLogIfMissing(questId, fallbackTitle, fallbackObjectives);
                        triggerQuestAcceptResync(questId, npcGuid, "already-on-quest");
                    } else if (failReason == 18) {
                        triggerQuestAcceptResync(questId, npcGuid, "already-completed");
                    }
                    clearPendingQuestAccept(questId);
                }
            }
            // The reason in chat as 0x006cd7c0 picks it - every reason,
            // "already on" and "already done" included. This was English
            // ("Quest unavailable: ...") and left those two out.
            const char* key = "ERR_QUEST_NEED_PREREQS";
            const char* enUS = "You don't meet the requirements for that quest.";
            switch (failReason) {
                case 1:    key = "ERR_QUEST_FAILED_LOW_LEVEL"; enUS = "You are not high enough level for that quest."; break;
                case 6:    key = "ERR_QUEST_FAILED_WRONG_RACE"; enUS = "That quest is not available to your race."; break;
                case 7:    key = "ERR_QUEST_ALREADY_DONE"; enUS = "You have completed that quest."; break;
                case 0x0c: key = "ERR_QUEST_ONLY_ONE_TIMED"; enUS = "You can only be on one timed quest at a time."; break;
                case 0x0d:
                case 0x12: key = "ERR_QUEST_ALREADY_ON"; enUS = "You are already on that quest."; break;
                case 0x10: key = "ERR_QUEST_FAILED_EXPANSION"; enUS = "This quest requires an expansion enabled account."; break;
                case 0x15: key = "ERR_QUEST_FAILED_MISSING_ITEMS"; enUS = "You don't have the required items with you. Check storage."; break;
                case 0x17: key = "ERR_QUEST_FAILED_NOT_ENOUGH_MONEY"; enUS = "You don't have enough money for that quest."; break;
                case 0x1a: key = nullptr; break;
                case 0x1b: key = "ERR_QUEST_FAILED_CAIS"; enUS = "You cannot complete quests once you have reached tired time."; break;
                case 0x1d: key = "ERR_QUEST_ALREADY_DONE_DAILY"; enUS = "You have completed that daily quest today."; break;
                default: break;
            }
            owner_.addSystemChatMessage(key
                ? interfaceString(key, enUS)
                : interfaceFormat("ERR_QUEST_FAILED_TOO_MANY_DAILY_QUESTS_I",
                                  "You have already completed %d daily quests today", {"25"}));
        }
    };

    // ---- SMSG_QUESTGIVER_QUEST_COMPLETE ----
    table[Opcode::SMSG_QUESTGIVER_QUEST_COMPLETE] = [this](network::Packet& packet) {
        if (packet.hasRemaining(4)) {
            uint32_t questId = packet.readUInt32();
            LOG_INFO("Quest completed: questId=", questId);
            if (pendingTurnInQuestId_ == questId) {
                pendingTurnInQuestId_ = 0;
                pendingTurnInNpcGuid_ = 0;
                pendingTurnInRewardRequest_ = false;
            }
            // The turn-in lines 0x006d0e10 prints, in chat: ERR_QUEST_COMPLETE_S
            // with the title, then ERR_QUEST_REWARD_EXP_I and
            // ERR_QUEST_REWARD_MONEY_S for the experience and money the
            // WotLK packet carries after the id, then the honour (sent ten
            // times over), talent point and arena point lines in their own
            // chat types. (Its reward item line is not given here.)
            for (const auto& q : questLog_) {
                if (q.questId == questId && !q.title.empty()) {
                    owner_.addSystemChatMessage(questTitleText("ERR_QUEST_COMPLETE_S", "%s completed.", q.title));
                    break;
                }
            }
            if (!isClassicLikeExpansion() && !isActiveExpansion("tbc") && packet.hasRemaining(8)) {
                const uint32_t rewardXp = packet.readUInt32();
                const uint32_t rewardMoney = packet.readUInt32();
                if (rewardXp != 0) {
                    owner_.addSystemChatMessage(interfaceFormat("ERR_QUEST_REWARD_EXP_I",
                        "Experience gained: %d.", {std::to_string(rewardXp)}));
                }
                if (static_cast<int32_t>(rewardMoney) > 0) {
                    const std::string coins = interface_text::moneyText(rewardMoney,
                        [](const char* k, const char* e) { return interfaceString(k, e); });
                    owner_.addSystemChatMessage(interfaceFormat("ERR_QUEST_REWARD_MONEY_S",
                        "Received %s.", {coins}));
                }
                if (packet.hasRemaining(12)) {
                    const int32_t honor = static_cast<int32_t>(packet.readUInt32());
                    const int32_t talents = static_cast<int32_t>(packet.readUInt32());
                    const int32_t arena = static_cast<int32_t>(packet.readUInt32());
                    if (honor > 0) {
                        owner_.addLocalChatLine(ChatType::COMBAT_HONOR_GAIN,
                            interfaceFormat("COMBATLOG_HONORAWARD", "You have been awarded %d honor points.",
                                            {std::to_string(honor / 10)}));
                    }
                    if (talents > 0) {
                        owner_.addLocalChatLine(ChatType::MONEY,
                            interfaceFormat("LEVEL_UP_CHAR_POINTS", "You have gained %d talent points.",
                                            {std::to_string(talents)}));
                    }
                    if (arena > 0) {
                        owner_.addLocalChatLine(ChatType::ARENA_POINTS,
                            interfaceFormat("COMBATLOG_ARENAPOINTSAWARD", "You have been awarded %d arena points.",
                                            {std::to_string(arena)}));
                    }
                }
            }
            for (auto it = questLog_.begin(); it != questLog_.end(); ++it) {
                if (it->questId == questId) {
                    // Fire toast callback before erasing
                    if (owner_.questCompleteCallbackRef()) {
                        owner_.questCompleteCallbackRef()(questId, it->title);
                    }
                    // Play quest-complete sound
                    if (auto* ac = owner_.services().audioCoordinator) {
                        if (auto* sfx = ac->getUiSoundManager())
                            sfx->playQuestComplete();
                    }
                    questLog_.erase(it);
                    owner_.setQuestTracked(questId, false);
                    LOG_INFO("  Removed quest ", questId, " from quest log");
                    if (owner_.addonEventCallbackRef())
                        owner_.addonEventCallbackRef()("QUEST_TURNED_IN", {std::to_string(questId)});
                    break;
                }
            }
        }
        if (owner_.addonEventCallbackRef()) {
            owner_.addonEventCallbackRef()("QUEST_LOG_UPDATE", {});
            owner_.addonEventCallbackRef()("UNIT_QUEST_LOG_CHANGED", {"player"});
        }
        // Re-query all nearby quest giver NPCs so markers refresh
        requeryNearbyQuestGiverStatus();
    };

    // ---- SMSG_QUESTUPDATE_ADD_KILL ----
    table[Opcode::SMSG_QUESTUPDATE_ADD_KILL] = [this](network::Packet& packet) {
        size_t rem = packet.getRemainingSize();
        if (rem >= 12) {
            uint32_t questId = packet.readUInt32();
            clearPendingQuestAccept(questId);
            const uint32_t rawEntry = packet.readUInt32();
            uint32_t entry = normalizeQuestObjectiveEntry(rawEntry);
            uint32_t count = packet.readUInt32();
            uint32_t reqCount = 0;
            if (packet.hasRemaining(4)) {
                reqCount = packet.readUInt32();
            }

            LOG_INFO("Quest kill update: questId=", questId, " entry=", entry,
                     " count=", count, "/", reqCount);

            for (auto& quest : questLog_) {
                if (quest.questId == questId) {
                    if (reqCount == 0) {
                        auto it = quest.killCounts.find(entry);
                        if (it != quest.killCounts.end()) reqCount = it->second.second;
                    }
                    // Fall back to killObjectives (parsed from SMSG_QUEST_QUERY_RESPONSE).
                    if (reqCount == 0) {
                        for (const auto& obj : quest.killObjectives) {
                            if (obj.npcOrGoId == 0 || obj.required == 0) continue;
                            uint32_t objEntry = static_cast<uint32_t>(
                                obj.npcOrGoId > 0 ? obj.npcOrGoId : -obj.npcOrGoId);
                            if (objEntry == entry) {
                                reqCount = obj.required;
                                break;
                            }
                        }
                    }
                    // Some quests (e.g. escort/event quests) report kill credit updates without
                    // a corresponding objective count in SMSG_QUEST_QUERY_RESPONSE. Fall back to
                    // current count so the progress display shows "N/N" instead of "N/0".
                    if (reqCount == 0) reqCount = count;
                    count = std::min(count, reqCount);
                    quest.killCounts[entry] = {count, reqCount};

                    std::string creatureName = owner_.getCachedCreatureName(entry);
                    // On screen in yellow, as UI_INFO_MESSAGE (0x006d8030,
                    // case 0x199). This was an English chat line, "Title:
                    // Name 3/10".
                    const std::string progressMsg = questKillCreditText(owner_, rawEntry, count, reqCount);
                    if (!progressMsg.empty()) owner_.addUIInfoMessage(progressMsg);

                    if (owner_.questProgressCallbackRef()) {
                        owner_.questProgressCallbackRef()(quest.title, creatureName, count, reqCount);
                    }
                    if (owner_.addonEventCallbackRef()) {
                        // The log INDEX, not the quest id. QuestLog_OnEvent
                        // hands arg1 straight to GetNumQuestLeaderBoards and
                        // AddQuestWatch, both of which count positions in the
                        // log - so a quest id addressed whichever quest
                        // happened to sit at that position, or none.
                        owner_.addonEventCallbackRef()("QUEST_WATCH_UPDATE",
                            {std::to_string(questLogIndexOf(questId))});
                        owner_.addonEventCallbackRef()("QUEST_LOG_UPDATE", {});
                        owner_.addonEventCallbackRef()("UNIT_QUEST_LOG_CHANGED", {"player"});
                    }

                    LOG_INFO("Updated kill count for quest ", questId, ": ",
                             count, "/", reqCount);
                    // When this objective just finished, a quest may have become
                    // turn-in-able - refresh nearby giver markers (! → ?) now
                    // instead of only after leaving and re-entering the area.
                    if (reqCount > 0 && count >= reqCount) requeryNearbyQuestGiverStatus();
                    break;
                }
            }
        } else if (rem >= 4) {
            // Swapped mapping fallback: treat as QUESTUPDATE_COMPLETE packet.
            uint32_t questId = packet.readUInt32();
            clearPendingQuestAccept(questId);
            LOG_INFO("Quest objectives completed (compat via ADD_KILL): questId=", questId);
            for (auto& quest : questLog_) {
                if (quest.questId == questId) {
                    quest.complete = true;
                    // As SMSG_QUESTUPDATE_COMPLETE words it, below.
                    owner_.addUIInfoMessage(questObjectiveCompleteText(questId));
                    break;
                }
            }
            requeryNearbyQuestGiverStatus();
        }
    };

    // ---- SMSG_QUESTUPDATE_COMPLETE ----
    table[Opcode::SMSG_QUESTUPDATE_COMPLETE] = [this](network::Packet& packet) {
        size_t rem = packet.getRemainingSize();
        if (rem >= 12) {
            uint32_t questId = packet.readUInt32();
            clearPendingQuestAccept(questId);
            const uint32_t rawEntry = packet.readUInt32();
            uint32_t entry = normalizeQuestObjectiveEntry(rawEntry);
            uint32_t count = packet.readUInt32();
            uint32_t reqCount = 0;
            if (packet.hasRemaining(4)) reqCount = packet.readUInt32();
            if (reqCount == 0) reqCount = count;
            count = std::min(count, reqCount);
            LOG_INFO("Quest kill update (compat via COMPLETE): questId=", questId,
                     " entry=", entry, " count=", count, "/", reqCount);
            for (auto& quest : questLog_) {
                if (quest.questId == questId) {
                    quest.killCounts[entry] = {count, reqCount};
                    // A kill credit by its layout, so worded as one (see
                    // SMSG_QUESTUPDATE_ADD_KILL). This was "Title: 3/10" in
                    // chat.
                    const std::string progressMsg = questKillCreditText(owner_, rawEntry, count, reqCount);
                    if (!progressMsg.empty()) owner_.addUIInfoMessage(progressMsg);
                    announceQuestLogChanged(questId);
                    break;
                }
            }
        } else if (rem >= 4) {
            uint32_t questId = packet.readUInt32();
            clearPendingQuestAccept(questId);
            // Visible at the default log level. This is the answer to "I did
            // the thing and nothing happened": it separates a server that
            // never credited from a server that did and an interface that did
            // not show it, and there is no other way to tell those apart from
            // outside. One line per objective completed, which is rare.
            LOG_WARNING("Quest objectives completed: questId=", questId);

            for (auto& quest : questLog_) {
                if (quest.questId == questId) {
                    quest.complete = true;
                    // ERR_QUEST_OBJECTIVE_COMPLETE_S with the quest's EndText,
                    // or ERR_QUEST_UNKNOWN_COMPLETE, on screen in yellow
                    // (0x006d8030, case 0x198). This was "Quest Complete:
                    // Title" in chat.
                    owner_.addUIInfoMessage(questObjectiveCompleteText(questId));
                    LOG_INFO("Marked quest ", questId, " as complete");
                    announceQuestLogChanged(questId);
                    break;
                }
            }
            // The mark over the giver's head is now wrong, and this is the one
            // completion path that never said so. Every other one re-asks -
            // the kill counter when its objective fills, both item paths, and
            // the compatibility branch above - so a quest finished by this
            // opcode kept the grey "in progress" question mark on an NPC that
            // was ready to take the turn-in.
            //
            // Only stale for an NPC already spawned when the quest finished: a
            // giver is asked about as it spawns, so walking up to a fresh one
            // showed the right mark and walking back to a familiar one did
            // not. That is what made it look intermittent.
            requeryNearbyQuestGiverStatus();
        }
    };

    // ---- SMSG_QUEST_FORCE_REMOVE ----
    table[Opcode::SMSG_QUEST_FORCE_REMOVE] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(4)) {
            LOG_WARNING("SMSG_QUEST_FORCE_REMOVE/SET_REST_START too short");
            return;
        }
        uint32_t value = packet.readUInt32();

        // WotLK uses this opcode as SMSG_SET_REST_START
        if (!isClassicLikeExpansion() && !isActiveExpansion("tbc")) {
            bool nowResting = (value != 0);
            if (nowResting != owner_.isRestingRef()) {
                owner_.isRestingRef() = nowResting;
                // No line. "You are now resting" is no string the interface
                // has, and 3.3.5 itself registers this opcode as
                // SMSG_QUEST_FORCE_REMOVE (0x006d8370), not as a rest start.
                if (owner_.addonEventCallbackRef())
                    owner_.addonEventCallbackRef()("PLAYER_UPDATE_RESTING", {});
            }
            return;
        }

        // Classic/TBC: treat as QUEST_FORCE_REMOVE (uint32 questId)
        uint32_t questId = value;
        clearPendingQuestAccept(questId);
        pendingQuestQueryIds_.erase(questId);
        if (questId == 0) {
            return;
        }

        bool removed = false;
        std::string removedTitle;
        for (auto it = questLog_.begin(); it != questLog_.end(); ++it) {
            if (it->questId == questId) {
                removedTitle = it->title;
                questLog_.erase(it);
                removed = true;
                break;
            }
        }
        if (currentQuestDetails_.questId == questId) {
            questDetailsOpen_ = false;
            questDetailsOpenTime_ = std::chrono::steady_clock::time_point{};
            currentQuestDetails_ = QuestDetailsData{};
            removed = true;
        }
        if (currentQuestRequestItems_.questId == questId) {
            questRequestItemsOpen_ = false;
            currentQuestRequestItems_ = QuestRequestItemsData{};
            removed = true;
        }
        if (currentQuestOfferReward_.questId == questId) {
            questOfferRewardOpen_ = false;
            currentQuestOfferReward_ = QuestOfferRewardData{};
            removed = true;
        }
        if (removed) {
            owner_.setQuestTracked(questId, false);
            // ERR_QUEST_FORCE_REMOVED_S in chat, and nothing for a quest whose
            // title is not known (0x006d8370). This was English, with the id
            // standing in for a missing title.
            if (!removedTitle.empty()) {
                owner_.addSystemChatMessage(questTitleText("ERR_QUEST_FORCE_REMOVED_S",
                    "The quest %s has been removed from your quest log", removedTitle));
            }
            if (owner_.addonEventCallbackRef()) {
                owner_.addonEventCallbackRef()("QUEST_LOG_UPDATE", {});
                owner_.addonEventCallbackRef()("UNIT_QUEST_LOG_CHANGED", {"player"});
                owner_.addonEventCallbackRef()("QUEST_REMOVED", {std::to_string(questId)});
            }
        }
    };

    // ---- SMSG_QUEST_QUERY_RESPONSE ----
    table[Opcode::SMSG_QUEST_QUERY_RESPONSE] = [this](network::Packet& packet) {
        if (packet.getSize() < 8) {
            LOG_WARNING("SMSG_QUEST_QUERY_RESPONSE: packet too small (", packet.getSize(), " bytes)");
            return;
        }

        uint32_t questId = packet.readUInt32();
        packet.readUInt32(); // questMethod

        const uint8_t questLogStride = owner_.getPacketParsers()
                                           ? owner_.getPacketParsers()->questLogStride() : 5;
        const bool isClassicLayout = questLogStride <= 4;

        // questLevel is the third field (after questId + questMethod) in every
        // expansion's SMSG_QUEST_QUERY_RESPONSE; -1 = player-scaling (WotLK)
        constexpr size_t kQuestLevelOffset = 8;
        int32_t questLevel = 0;
        if (packet.getData().size() >= kQuestLevelOffset + 4) {
            questLevel = static_cast<int32_t>(network::wire::loadLE32(packet.getData(), kQuestLevelOffset));
            if (questLevel < -1 || questLevel > 255) questLevel = 0; // sanity: wrong layout
        }

        // ZoneOrSort follows questLevel: offset 12 in Classic/TBC, 16 in WotLK
        // (which inserted MinLevel before it). >0 = AreaTable zone id, <0 =
        // QuestSort.dbc category. Used to group the quest log by zone.
        int32_t zoneOrSort = 0;
        {
            const size_t zosOffset = (questLogStride >= 5) ? 16 : 12;
            if (packet.getData().size() >= zosOffset + 4) {
                zoneOrSort = static_cast<int32_t>(network::wire::loadLE32(packet.getData(), zosOffset));
                if (zoneOrSort < -100000 || zoneOrSort > 100000) zoneOrSort = 0; // sanity
            }
        }

        const QuestQueryTextCandidate parsed = pickBestQuestQueryTexts(packet.getData(), isClassicLayout);
        const QuestQueryObjectives objs = extractQuestQueryObjectives(packet.getData(), questLogStride);
        // What the text and objective parses actually found, at warning so it
        // survives the default log. Two reports meet here: a blank description
        // panel, and no objectives shown on screen or in the log. The text
        // parse feeds the description; the objective parse feeds the leader
        // boards the watch frame and log draw. Naming both - the description's
        // raw third string, and how many kill and item objectives came out
        // valid - says which parse missed on this realm. One line per query.
        int objKills = 0, objItems = 0;
        if (objs.valid) {
            for (const auto& k : objs.kills)
                if (k.npcOrGoId != 0 || k.required > 0) ++objKills;
            for (const auto& it : objs.items)
                if (it.itemId != 0 || it.required > 0) ++objItems;
        }
        // At debug now. This was raised to warning for two reports - a blank
        // description panel and no objectives on screen - and both are answered:
        // the string block starts sixty-five fields in, not fifty-seven, and a
        // session's worth of quests now parse with a real title, real
        // objectives and a real description every time. Kept, because it is the
        // thing that showed the fault, and one WOWEE_LOG_LEVEL=debug away.
        LOG_DEBUG("Quest text parse: id=", questId,
                    " classicLayout=", isClassicLayout ? "yes" : "no",
                    " title=\"", parsed.title.substr(0, 40),
                    "\" (", parsed.title.size(), " ch) objectives=",
                    parsed.objectives.size(), "ch description=",
                    parsed.description.size(), "ch completion=",
                    parsed.completionText.size(), "ch score=", parsed.score,
                    " thirdRaw(", parsed.debugThirdRaw.size(), ")=\"",
                    parsed.debugThirdRaw.substr(0, 50), "\"",
                    " objParse: valid=", objs.valid ? 1 : 0,
                    " kills=", objKills, " items=", objItems);
        const QuestQueryRewardsData rwds = QuestQueryRewardsParser::parse(packet.getData(), questLogStride);
        if (!parsed.endText.empty()) questEndTexts()[questId] = parsed.endText;

        for (auto& q : questLog_) {
            if (q.questId != questId) continue;

            if (questLevel != 0) q.level = questLevel;
            if (zoneOrSort != 0) q.zoneOrSort = zoneOrSort;

            const int existingScore = scoreQuestTitle(q.title);
            const bool parsedStrong = isStrongQuestTitle(parsed.title);
            const bool parsedLongEnough = parsed.title.size() >= 6;
            const bool notShorterThanExisting =
                isPlaceholderQuestTitle(q.title) || q.title.empty() || parsed.title.size() + 2 >= q.title.size();
            const bool shouldReplaceTitle =
                parsed.score > -1000 &&
                parsedStrong &&
                parsedLongEnough &&
                notShorterThanExisting &&
                (isPlaceholderQuestTitle(q.title) || q.title.empty() || parsed.score >= existingScore + 12);

            if (shouldReplaceTitle && !parsed.title.empty()) {
                q.title = parsed.title;
            }
            if (!parsed.objectives.empty() &&
                (q.objectives.empty() || q.objectives.size() < 16)) {
                q.objectives = parsed.objectives;
            }
            if (!parsed.completionText.empty()) q.completionText = parsed.completionText;
            if (!parsed.description.empty()) q.description = parsed.description;

            // Store structured kill/item objectives for later kill-count restoration.
            if (objs.valid) {
                std::unordered_set<uint32_t> validKillKeys;
                std::unordered_set<uint32_t> validItemKeys;
                for (int i = 0; i < 4; ++i) {
                    q.killObjectives[i].npcOrGoId = objs.kills[i].npcOrGoId;
                    q.killObjectives[i].required  = objs.kills[i].required;
                    q.objectiveSourceItems[i] = objs.kills[i].sourceItem;
                    if (objs.kills[i].npcOrGoId != 0 && objs.kills[i].required != 0) {
                        validKillKeys.insert(static_cast<uint32_t>(
                            objs.kills[i].npcOrGoId > 0 ? objs.kills[i].npcOrGoId
                                                       : -objs.kills[i].npcOrGoId));
                    }
                }
                for (int i = 0; i < 6; ++i) {
                    q.itemObjectives[i].itemId   = objs.items[i].itemId;
                    q.itemObjectives[i].required = objs.items[i].required;
                    if (objs.items[i].itemId != 0 && objs.items[i].required != 0)
                        validItemKeys.insert(objs.items[i].itemId);
                }
                // Remove entries produced by an earlier malformed/heuristic decode while
                // retaining live progress for objectives that are still part of the quest.
                std::erase_if(q.killCounts, [&](const auto& value) {
                    return validKillKeys.count(value.first) == 0;
                });
                std::erase_if(q.itemCounts, [&](const auto& value) {
                    return validItemKeys.count(value.first) == 0;
                });
                std::erase_if(q.requiredItemCounts, [&](const auto& value) {
                    return validItemKeys.count(value.first) == 0;
                });
                applyPackedKillCountsFromFields(q);
                for (int i = 0; i < 4; ++i) {
                    int32_t id = objs.kills[i].npcOrGoId;
                    if (id == 0 || objs.kills[i].required == 0) continue;
                    if (id > 0) owner_.queryCreatureInfo(static_cast<uint32_t>(id), 0);
                    else        owner_.queryGameObjectInfo(static_cast<uint32_t>(-id), 0);
                }
                for (int i = 0; i < 6; ++i) {
                    if (objs.items[i].itemId != 0 && objs.items[i].required != 0)
                        owner_.queryItemInfo(objs.items[i].itemId, 0);
                }
                LOG_DEBUG("Quest ", questId, " objectives parsed: kills=[",
                          objs.kills[0].npcOrGoId, "/", objs.kills[0].required, ", ",
                          objs.kills[1].npcOrGoId, "/", objs.kills[1].required, ", ",
                          objs.kills[2].npcOrGoId, "/", objs.kills[2].required, ", ",
                          objs.kills[3].npcOrGoId, "/", objs.kills[3].required, "]");
            }

            // Store reward data and pre-fetch item info for icons.
            if (rwds.valid) {
                q.rewardMoney = rwds.rewardMoney;
                if (rwds.rewardSpellId != 0) q.rewardSpellId = rwds.rewardSpellId;
                if (rwds.xpId != 0) q.rewardXPId = rwds.xpId;
                q.rewardHonor = rwds.rewardHonor;
                q.rewardTalents = rwds.bonusTalents;
                q.rewardArenaPoints = rwds.arenaPoints;
                if (rwds.rewardTitleId != 0) q.rewardTitleId = rwds.rewardTitleId;
                for (size_t i = 0; i < 5; ++i) {
                    q.factionRewards[i].factionId = rwds.factionId[i];
                    q.factionRewards[i].valueId   = rwds.factionValueId[i];
                    q.factionRewards[i].override  = rwds.factionValueOverride[i];
                }
                q.sourceItemId = rwds.sourceItemId;
                q.questFlags = rwds.questFlags;
                q.playersSlain = rwds.playersSlain;
                q.repObjectiveFaction = rwds.repObjectiveFaction;
                q.repObjectiveFaction2 = rwds.repObjectiveFaction2;
                // The watch frame needs its name and icon to draw a button.
                if (q.sourceItemId != 0) owner_.queryItemInfo(q.sourceItemId, 0);
                for (int i = 0; i < 4; ++i) {
                    q.rewardItems[i].itemId = rwds.itemId[i];
                    q.rewardItems[i].count  = (rwds.itemId[i] != 0) ? rwds.itemCount[i] : 0;
                    if (rwds.itemId[i] != 0) owner_.queryItemInfo(rwds.itemId[i], 0);
                }
                for (int i = 0; i < 6; ++i) {
                    q.rewardChoiceItems[i].itemId = rwds.choiceItemId[i];
                    q.rewardChoiceItems[i].count  = (rwds.choiceItemId[i] != 0) ? rwds.choiceItemCount[i] : 0;
                    if (rwds.choiceItemId[i] != 0) owner_.queryItemInfo(rwds.choiceItemId[i], 0);
                }
            }

            // The response arrives a beat after the log panel already drew,
            // so the title, objectives and description it just filled in reach
            // a quest the detail pane rendered while they were still blank.
            // QuestLog_UpdateQuestDetails runs on QUEST_LOG_UPDATE for the
            // selected quest, so firing it here repaints the pane with the text
            // that just landed - without it a queried quest keeps its empty
            // description until the log is closed and reopened, which is the
            // "only after a relog" shape reported for the description panel.
            owner_.addonEventCallbackRef()("QUEST_LOG_UPDATE", {});
            break;
        }

        pendingQuestQueryIds_.erase(questId);
    };

    // ---- SMSG_QUESTUPDATE_ADD_PVP_KILL ----
    table[Opcode::SMSG_QUESTUPDATE_ADD_PVP_KILL] = [this](network::Packet& packet) {
        if (packet.hasRemaining(12)) {
            uint32_t questId = packet.readUInt32();
            uint32_t count   = packet.readUInt32();
            uint32_t reqCount = packet.readUInt32();

            constexpr uint32_t PVP_KILL_ENTRY = 0u;
            for (auto& quest : questLog_) {
                if (quest.questId != questId) continue;

                if (reqCount == 0) {
                    auto it = quest.killCounts.find(PVP_KILL_ENTRY);
                    if (it != quest.killCounts.end()) reqCount = it->second.second;
                }
                if (reqCount == 0) {
                    for (const auto& obj : quest.killObjectives) {
                        if (obj.npcOrGoId == 0 && obj.required > 0) {
                            reqCount = obj.required;
                            break;
                        }
                    }
                }
                if (reqCount == 0) reqCount = count;
                count = std::min(count, reqCount);
                quest.killCounts[PVP_KILL_ENTRY] = {count, reqCount};

                // ERR_QUEST_ADD_PLAYER_KILL_SII on screen in yellow
                // (0x006d8030, case 0x46f). This was "Title: PvP kills 3/10"
                // in chat.
                owner_.addUIInfoMessage(interfaceFormat("ERR_QUEST_ADD_PLAYER_KILL_SII",
                    "Players slain: %d/%d", {std::to_string(count), std::to_string(reqCount)}));
                break;
            }
        }
    };

    // ---- Completed quests response (moved from GameHandler) ----
    // 0x005b5190: the list replaced by the one sent - a count and that many
    // quest ids - then QUEST_QUERY_COMPLETE (0x296).
    table[Opcode::SMSG_QUERY_QUESTS_COMPLETED_RESPONSE] = [this](network::Packet& packet) {
        if (packet.hasRemaining(4)) {
            uint32_t count = packet.readUInt32();
            if (count <= packet.getRemainingSize() / 4) {
                auto& completed = owner_.completedQuestsRef();
                completed.clear();
                for (uint32_t i = 0; i < count; ++i) completed.insert(packet.readUInt32());
                LOG_DEBUG("SMSG_QUERY_QUESTS_COMPLETED_RESPONSE: ", count, " completed quests");
                owner_.fireAddonEvent("QUEST_QUERY_COMPLETE", {});
            }
        }
        packet.skipAll();
    };
}

// ---------------------------------------------------------------------------
// Public API methods
// ---------------------------------------------------------------------------

void QuestHandler::selectGossipOption(uint32_t optionId, const std::string& code) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket() || !gossipWindowOpen_) return;
    LOG_INFO("selectGossipOption: optionId=", optionId,
             " npcGuid=0x", std::hex, currentGossip_.npcGuid, std::dec,
             " menuId=", currentGossip_.menuId,
             " numOptions=", currentGossip_.options.size());
    // The code goes on the wire only for an option the server flagged coded -
    // it reads the string conditionally, on GossipOptionCoded, so sending one
    // for an ordinary option leaves a trailing string it never consumes.
    auto packet = GossipSelectOptionPacket::build(currentGossip_.npcGuid,
                                                  currentGossip_.menuId, optionId, code);
    owner_.getSocket()->send(packet);

    for (const auto& opt : currentGossip_.options) {
        if (opt.id != optionId) continue;
        LOG_INFO("  matched option: id=", opt.id, " icon=", (int)opt.icon, " text='", opt.text, "'");

        std::string text = opt.text;
        std::string textLower = text;
        std::transform(textLower.begin(), textLower.end(), textLower.begin(),
                       [](unsigned char c){ return static_cast<char>(std::tolower(c)); });

        // Icon- and text-based NPC interaction fallbacks.
        // Use flags to avoid sending the same activation packet twice when
        // both the icon and text match (e.g., banker icon 6 + "deposit box").
        bool sentBanker = false;
        bool sentAuction = false;

        if (opt.icon == 6) {
            auto pkt = BankerActivatePacket::build(currentGossip_.npcGuid);
            owner_.getSocket()->send(pkt);
            sentBanker = true;
            LOG_INFO("Sent CMSG_BANKER_ACTIVATE (icon) for npc=0x", std::hex, currentGossip_.npcGuid, std::dec);
        }

        if (!sentAuction && (text == "GOSSIP_OPTION_AUCTIONEER" || textLower.find("auction") != std::string::npos)) {
            auto pkt = AuctionHelloPacket::build(currentGossip_.npcGuid);
            owner_.getSocket()->send(pkt);
            sentAuction = true;
            LOG_INFO("Sent MSG_AUCTION_HELLO for npc=0x", std::hex, currentGossip_.npcGuid, std::dec);
        }

        if (!sentBanker && (text == "GOSSIP_OPTION_BANKER" || textLower.find("deposit box") != std::string::npos)) {
            auto pkt = BankerActivatePacket::build(currentGossip_.npcGuid);
            owner_.getSocket()->send(pkt);
            sentBanker = true;
            LOG_INFO("Sent CMSG_BANKER_ACTIVATE (text) for npc=0x", std::hex, currentGossip_.npcGuid, std::dec);
        }

        const bool isVendor = (text == "GOSSIP_OPTION_VENDOR" ||
                               (textLower.find("browse") != std::string::npos &&
                                (textLower.find("goods") != std::string::npos || textLower.find("wares") != std::string::npos)));
        const bool isArmorer = (text == "GOSSIP_OPTION_ARMORER" || textLower.find("repair") != std::string::npos);
        if (isVendor || isArmorer) {
            if (isArmorer) {
                owner_.setVendorCanRepair(true);
            }
            auto pkt = ListInventoryPacket::build(currentGossip_.npcGuid);
            owner_.getSocket()->send(pkt);
            LOG_DEBUG("Sent CMSG_LIST_INVENTORY (gossip) to npc=0x", std::hex, currentGossip_.npcGuid, std::dec);
        }

        // Only while this client draws the gossip window. It has no confirm
        // step, so matching the option's text and sending the activate is the
        // whole of how a hearthstone gets set there.
        //
        // FrameXML follows the server's own flow instead - the select goes out,
        // the server asks with SMSG_BINDER_CONFIRM, the popup answers - so
        // sending here as well would bind before the question was asked and
        // then ask it anyway.
        if (!ui::frameXmlOwns(ui::UiElement::Gossip) &&
            (textLower.find("make this inn your home") != std::string::npos ||
             textLower.find("set your home") != std::string::npos)) {
            auto bindPkt = BinderActivatePacket::build(currentGossip_.npcGuid);
            owner_.getSocket()->send(bindPkt);
            LOG_INFO("Sent CMSG_BINDER_ACTIVATE for npc=0x", std::hex, currentGossip_.npcGuid, std::dec);
        }

        // Stable master detection
        if (text == "GOSSIP_OPTION_STABLE" ||
            textLower.find("stable") != std::string::npos ||
            textLower.find("my pet") != std::string::npos) {
            owner_.stableMasterGuidRef() = currentGossip_.npcGuid;
            owner_.stableWindowOpenRef() = false;
            auto listPkt = ListStabledPetsPacket::build(currentGossip_.npcGuid);
            owner_.getSocket()->send(listPkt);
            LOG_INFO("Sent MSG_LIST_STABLED_PETS (gossip) to npc=0x",
                     std::hex, currentGossip_.npcGuid, std::dec);
        }
        break;
    }
}

void QuestHandler::selectGossipQuest(uint32_t questId) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket() || !gossipWindowOpen_) return;

    const QuestLogEntry* activeQuest = nullptr;
    for (const auto& q : questLog_) {
        if (q.questId == questId) {
            activeQuest = &q;
            break;
        }
    }

    // Validate against server-auth quest slot fields
    auto questInServerLogSlots = [&](uint32_t qid) -> bool {
        if (qid == 0 || owner_.lastPlayerFieldsRef().empty()) return false;
        const uint16_t ufQuestStart = fieldIndex(UF::PLAYER_QUEST_LOG_START);
        const uint8_t qStride = owner_.getPacketParsers() ? owner_.getPacketParsers()->questLogStride() : 5;
        const uint16_t ufQuestEnd = ufQuestStart + 25 * qStride;
        for (const auto& [key, val] : owner_.lastPlayerFieldsRef()) {
            if (key < ufQuestStart || key >= ufQuestEnd) continue;
            if ((key - ufQuestStart) % qStride != 0) continue;
            if (val == qid) return true;
        }
        return false;
    };
    const bool questInServerLog = questInServerLogSlots(questId);
    if (questInServerLog && !activeQuest) {
        addQuestToLocalLogIfMissing(questId, "Quest #" + std::to_string(questId), "");
        requestQuestQuery(questId, false);
        for (const auto& q : questLog_) {
            if (q.questId == questId) {
                activeQuest = &q;
                break;
            }
        }
    }
    const bool activeQuestConfirmedByServer = questInServerLog;
    const bool shouldStartProgressFlow = activeQuestConfirmedByServer;
    if (shouldStartProgressFlow) {
        pendingTurnInQuestId_ = questId;
        pendingTurnInNpcGuid_ = currentGossip_.npcGuid;
        pendingTurnInRewardRequest_ = activeQuest ? activeQuest->complete : false;
        auto packet = QuestgiverCompleteQuestPacket::build(currentGossip_.npcGuid, questId);
        owner_.getSocket()->send(packet);
    } else {
        pendingTurnInQuestId_ = 0;
        pendingTurnInNpcGuid_ = 0;
        pendingTurnInRewardRequest_ = false;
        auto packet = owner_.getPacketParsers()
            ? owner_.getPacketParsers()->buildQueryQuestPacket(currentGossip_.npcGuid, questId)
            : QuestgiverQueryQuestPacket::build(currentGossip_.npcGuid, questId);
        owner_.getSocket()->send(packet);
    }

    gossipWindowOpen_ = false;
}

bool QuestHandler::requestQuestQuery(uint32_t questId, bool force) {
    if (questId == 0 || owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return false;
    if (!force && pendingQuestQueryIds_.count(questId)) return false;

    network::Packet pkt(wireOpcode(Opcode::CMSG_QUEST_QUERY));
    pkt.writeUInt32(questId);
    owner_.getSocket()->send(pkt);
    pendingQuestQueryIds_.insert(questId);

    // WotLK supports CMSG_QUEST_POI_QUERY to get objective map locations.
    if (owner_.getPacketParsers() && owner_.getPacketParsers()->questLogStride() == 5) {
        const uint32_t wirePoiQuery = wireOpcode(Opcode::CMSG_QUEST_POI_QUERY);
        if (wirePoiQuery != 0xFFFF) {
            network::Packet poiPkt(static_cast<uint16_t>(wirePoiQuery));
            poiPkt.writeUInt32(1);          // count = 1
            poiPkt.writeUInt32(questId);
            owner_.getSocket()->send(poiPkt);
        }
    }
    return true;
}

void QuestHandler::acceptQuest() {
    if (!questDetailsOpen_ || owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    const uint32_t questId = currentQuestDetails_.questId;
    if (questId == 0) return;
    uint64_t npcGuid = currentQuestDetails_.npcGuid;
    if (pendingQuestAcceptTimeouts_.count(questId) != 0) {
        LOG_DEBUG("Ignoring duplicate quest accept while pending: questId=", questId);
        triggerQuestAcceptResync(questId, npcGuid, "duplicate-accept");
        dismissQuestDetails();
        return;
    }
    const bool inLocalLog = hasQuestInLog(questId);
    const int serverSlot = findQuestLogSlotIndexFromServer(questId);
    if (serverSlot >= 0) {
        LOG_INFO("Quest already in server quest log: questId=", questId,
                 " slot=", serverSlot, " inLocalLog=", inLocalLog);
        // Ensure it's in our local log even if server already has it
        addQuestToLocalLogIfMissing(questId, currentQuestDetails_.title, currentQuestDetails_.objectives);
        requestQuestQuery(questId, false);
        // Re-query NPC status from server
        if (npcGuid && owner_.getSocket()) {
            network::Packet qsPkt(wireOpcode(Opcode::CMSG_QUESTGIVER_STATUS_QUERY));
            qsPkt.writeUInt64(npcGuid);
            owner_.getSocket()->send(qsPkt);
        }
        dismissQuestDetails();
        return;
    }
    if (inLocalLog) {
        LOG_WARNING("Quest accept local/server mismatch, allowing re-accept: questId=", questId);
        std::erase_if(questLog_, [&](const QuestLogEntry& q) { return q.questId == questId; });
    }

    // The server caps the quest log (20 slots in Vanilla, 25 in TBC/WotLK).
    // Accepting past the cap only earns SMSG_QUESTLOG_FULL, so stop it here
    // with a clearer message and no optimistic local entry to roll back.
    const int maxSlots = maxQuestLogSlots();
    if (static_cast<int>(questLog_.size()) >= maxSlots) {
        // The ERR_QUEST_LOG_FULL the server would answer with, on screen only
        // (0x006d7f10, case 0x195) - the original has no check of its own here
        // and lets the server refuse. This was an English line in chat too.
        owner_.addUIError(interfaceString("ERR_QUEST_LOG_FULL", "Your quest log is full."));
        if (auto* ac = owner_.services().audioCoordinator)
            if (auto* sfx = ac->getUiSoundManager()) sfx->playError();
        return;
    }

    network::Packet packet = owner_.getPacketParsers()
        ? owner_.getPacketParsers()->buildAcceptQuestPacket(npcGuid, questId)
        : QuestgiverAcceptQuestPacket::build(npcGuid, questId);
    owner_.getSocket()->send(packet);
    pendingQuestAcceptTimeouts_[questId] = 5.0f;
    pendingQuestAcceptNpcGuids_[questId] = npcGuid;

    // Immediately add to local quest log using available details
    addQuestToLocalLogIfMissing(questId, currentQuestDetails_.title, currentQuestDetails_.objectives);

    // Auto-track newly accepted quests so their objectives appear in the
    // on-screen tracker right away (matches retail behavior). Only the
    // player-driven accept path lands here - quests loaded on login/resync go
    // through addQuestToLocalLogIfMissing without tracking, so the tracker's
    // "show all when nothing tracked" fallback still applies to those.
    owner_.setQuestTracked(questId, true);

    // Play quest-accept sound
    if (auto* ac = owner_.services().audioCoordinator) {
        if (auto* sfx = ac->getUiSoundManager())
            sfx->playQuestActivate();
    }

    dismissQuestDetails();

    // Re-query quest giver status so marker updates (! → ?)
    if (npcGuid) {
        network::Packet qsPkt(wireOpcode(Opcode::CMSG_QUESTGIVER_STATUS_QUERY));
        qsPkt.writeUInt64(npcGuid);
        owner_.getSocket()->send(qsPkt);
    }
}

void QuestHandler::dismissQuestDetails(bool announce) {
    questDetailsOpen_ = false;
    questDetailsOpenTime_ = std::chrono::steady_clock::time_point{};
    currentQuestDetails_ = QuestDetailsData{};
    // The interface's quest frame opens on QUEST_DETAIL and closes on this and
    // nothing else, so an accept that only reset the state left FrameXML's
    // window open over the world while this client's own window - which reads
    // isQuestDetailsOpen directly - closed.
    //
    // Not when the interface is the one closing it. QuestFrame_OnHide calls
    // CloseQuest, and answering that with the event that hides QuestFrame is
    // an echo: the panel is told to close by its own closing. It is absorbed
    // today only because OnHide is deferred a frame, and it is the reason
    // OnHide cannot simply be made synchronous.
    if (announce && owner_.addonEventCallbackRef()) {
        owner_.addonEventCallbackRef()("QUEST_FINISHED", {});
    }
}

void QuestHandler::declineQuest(bool announce) {
    // Declining and dismissing leave the client in the same state and always
    // did, character for character.
    dismissQuestDetails(announce);
}

void QuestHandler::closeGossip() {
    gossipWindowOpen_ = false;
    if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("GOSSIP_CLOSED", {});
    currentGossip_ = GossipMessageData{};
    questGreeting_.clear();
}

void QuestHandler::offerQuestFromItem(uint64_t itemGuid, uint32_t questId) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    if (itemGuid == 0 || questId == 0) {
        // Nothing said: there is no such line in the interface.
        return;
    }
    // Send CMSG_QUESTGIVER_QUERY_QUEST with the item GUID as the "questgiver."
    // The server responds with SMSG_QUESTGIVER_QUEST_DETAILS which handleQuestDetails()
    // picks up and opens the Accept/Decline dialog.
    auto queryPkt = owner_.getPacketParsers()
        ? owner_.getPacketParsers()->buildQueryQuestPacket(itemGuid, questId)
        : QuestgiverQueryQuestPacket::build(itemGuid, questId);
    owner_.getSocket()->send(queryPkt);
    LOG_INFO("offerQuestFromItem: itemGuid=0x", std::hex, itemGuid, std::dec,
             " questId=", questId);
}

void QuestHandler::completeQuest() {
    if (!questRequestItemsOpen_ || owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    pendingTurnInQuestId_ = currentQuestRequestItems_.questId;
    pendingTurnInNpcGuid_ = currentQuestRequestItems_.npcGuid;
    pendingTurnInRewardRequest_ = currentQuestRequestItems_.isCompletable();

    LOG_DEBUG("completeQuest: questId=", currentQuestRequestItems_.questId,
              " completable=", currentQuestRequestItems_.isCompletable());

    auto packet = QuestgiverCompleteQuestPacket::build(
        currentQuestRequestItems_.npcGuid, currentQuestRequestItems_.questId);
    owner_.getSocket()->send(packet);
    questRequestItemsOpen_ = false;
    currentQuestRequestItems_ = QuestRequestItemsData{};
}

void QuestHandler::closeQuestRequestItems(bool announce) {
    pendingTurnInRewardRequest_ = false;
    questRequestItemsOpen_ = false;
    currentQuestRequestItems_ = QuestRequestItemsData{};
    // What closes the quest frame. It opens on QUEST_DETAIL, QUEST_PROGRESS or
    // QUEST_COMPLETE and hides on this, so without it the window would open and
    // stay open over whatever came next - unless the interface is the one
    // closing it, in which case see dismissQuestDetails.
    if (announce && owner_.addonEventCallbackRef()) {
        owner_.addonEventCallbackRef()("QUEST_FINISHED", {});
    }
}

void QuestHandler::chooseQuestReward(uint32_t rewardIndex) {
    if (!questOfferRewardOpen_ || owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    uint64_t npcGuid = currentQuestOfferReward_.npcGuid;
    LOG_INFO("Completing quest: questId=", currentQuestOfferReward_.questId,
             " npcGuid=", npcGuid, " rewardIndex=", rewardIndex);
    auto packet = QuestgiverChooseRewardPacket::build(
        npcGuid, currentQuestOfferReward_.questId, rewardIndex);
    owner_.getSocket()->send(packet);
    pendingTurnInQuestId_ = 0;
    pendingTurnInNpcGuid_ = 0;
    // Announced, which is what closes the dialog. Taking the reward put the
    // three fields back and told nothing: this client's own window reads the
    // flag and closed, and the interface's quest frame hides on QUEST_FINISHED
    // and nothing else, so it stayed on screen over the world after the quest
    // had been handed in.
    closeQuestOfferReward(true);

    // Re-query quest giver status so markers update
    if (npcGuid) {
        network::Packet qsPkt(wireOpcode(Opcode::CMSG_QUESTGIVER_STATUS_QUERY));
        qsPkt.writeUInt64(npcGuid);
        owner_.getSocket()->send(qsPkt);
    }
}

void QuestHandler::closeQuestOfferReward(bool announce) {
    pendingTurnInRewardRequest_ = false;
    questOfferRewardOpen_ = false;
    currentQuestOfferReward_ = QuestOfferRewardData{};
    if (announce && owner_.addonEventCallbackRef()) {
        owner_.addonEventCallbackRef()("QUEST_FINISHED", {});
    }
}

void QuestHandler::abandonQuest(uint32_t questId) {
    clearPendingQuestAccept(questId);
    int localIndex = -1;
    for (size_t i = 0; i < questLog_.size(); ++i) {
        if (questLog_[i].questId == questId) {
            localIndex = static_cast<int>(i);
            break;
        }
    }

    int slotIndex = findQuestLogSlotIndexFromServer(questId);
    if (slotIndex < 0 && localIndex >= 0) {
        slotIndex = localIndex;
        LOG_WARNING("Abandon quest using local slot fallback: questId=", questId, " slot=", slotIndex);
    }

    if (slotIndex >= 0 && slotIndex < 25) {
        if (owner_.getState() == WorldState::IN_WORLD && owner_.getSocket()) {
            network::Packet pkt(wireOpcode(Opcode::CMSG_QUESTLOG_REMOVE_QUEST));
            pkt.writeUInt8(static_cast<uint8_t>(slotIndex));
            owner_.getSocket()->send(pkt);
        }
    } else {
        LOG_WARNING("Abandon quest failed: no quest-log slot found for questId=", questId);
    }

    if (localIndex >= 0) {
        questLog_.erase(questLog_.begin() + static_cast<ptrdiff_t>(localIndex));
        if (owner_.addonEventCallbackRef()) {
            owner_.addonEventCallbackRef()("QUEST_LOG_UPDATE", {});
            owner_.addonEventCallbackRef()("UNIT_QUEST_LOG_CHANGED", {"player"});
            owner_.addonEventCallbackRef()("QUEST_REMOVED", {std::to_string(questId)});
        }
    }

    // Re-query nearby quest giver NPCs so markers refresh (e.g. "?" → "!")
    if (owner_.getSocket()) {
        for (const auto& [guid, entity] : owner_.getEntityManager().getEntities()) {
            if (entity->getType() != ObjectType::UNIT) continue;
            auto unit = std::static_pointer_cast<Unit>(entity);
            if (unit->getNpcFlags() & NPC_FLAG_QUESTGIVER) {
                network::Packet qsPkt(wireOpcode(Opcode::CMSG_QUESTGIVER_STATUS_QUERY));
                qsPkt.writeUInt64(guid);
                owner_.getSocket()->send(qsPkt);
            }
        }
    }

    // Remove any quest POI minimap markers for this quest.
    gossipPois_.erase(
        std::remove_if(gossipPois_.begin(), gossipPois_.end(),
            [questId](const GossipPoi& p) { return p.data == questId; }),
        gossipPois_.end());
}

void QuestHandler::shareQuestWithParty(uint32_t questId) {
    // Silent, as QuestLogPushQuest (0x005e4ed0) is: it sends or does nothing,
    // and what the player hears back is MSG_QUEST_PUSH_RESULT's
    // ERR_QUEST_PUSH_*_S. This printed English lines of its own - "Sharing
    // quest: X", "Quest shared.", and two refusals.
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    if (!owner_.isInGroup()) return;
    network::Packet pkt(wireOpcode(Opcode::CMSG_PUSHQUESTTOPARTY));
    pkt.writeUInt32(questId);
    owner_.getSocket()->send(pkt);
}

void QuestHandler::acceptSharedQuest() {
    if (!pendingSharedQuest_ || !owner_.getSocket()) return;
    pendingSharedQuest_ = false;
    network::Packet pkt(wireOpcode(Opcode::CMSG_QUEST_CONFIRM_ACCEPT));
    pkt.writeUInt32(sharedQuestId_);
    owner_.getSocket()->send(pkt);
    // Auto-track the accepted shared quest, same as a normal accept.
    owner_.setQuestTracked(sharedQuestId_, true);
    // No line: ConfirmAcceptQuest (0x0058c910) only sends. The quest's own
    // ERR_QUEST_ACCEPTED_S comes when it reaches the log.
}

void QuestHandler::declineSharedQuest() {
    pendingSharedQuest_ = false;
    // No response packet needed - just dismiss the UI
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

int QuestHandler::maxQuestLogSlots() const {
    return isClassicLikeExpansion() ? 20 : 25;
}

const std::string& QuestHandler::getQuestSortName(uint32_t sortId) {
    static const std::string kEmpty;
    if (!questSortDbcLoaded_) {
        questSortDbcLoaded_ = true;
        auto* am = owner_.services().assetManager;
        if (am && am->isInitialized()) {
            auto dbc = am->loadDBC("QuestSort.dbc");
            // ID(0) + Name locstring whose English text sits at field 1
            if (dbc && dbc->isLoaded() && dbc->getFieldCount() >= 2) {
                for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
                    uint32_t id = dbc->getUInt32(i, 0);
                    std::string name = dbc->getString(i, 1);
                    if (id != 0 && !name.empty())
                        questSortNames_[id] = std::move(name);
                }
                LOG_INFO("Loaded ", questSortNames_.size(), " quest sort names");
            }
        }
    }
    auto it = questSortNames_.find(sortId);
    return it != questSortNames_.end() ? it->second : kEmpty;
}

uint32_t QuestHandler::getQuestRewardXP(int32_t level, uint32_t xpDifficulty) {
    // xpDifficulty is AzerothCore's RewardXPDifficulty, indexing the ten Exp
    // columns directly (Exp[d] = column d = row[d] here). Index 0's column is
    // zero at every level, and an unparsed layout leaves the index at 0 too, so
    // both correctly answer no reward XP. Valid difficulty indices are 0..9.
    if (level <= 0 || xpDifficulty > 9) return 0;
    if (!questXpDbcLoaded_) {
        questXpDbcLoaded_ = true;
        auto* am = owner_.services().assetManager;
        if (am && am->isInitialized()) {
            auto dbc = am->loadDBC("QuestXP.dbc");
            // Field 0 is the level; fields 1..10 are the ten difficulty columns.
            if (dbc && dbc->isLoaded() && dbc->getFieldCount() >= 11) {
                for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
                    const int32_t lvl = static_cast<int32_t>(dbc->getUInt32(i, 0));
                    std::array<uint32_t, 10> row{};
                    for (uint32_t c = 0; c < 10; ++c) row[c] = dbc->getUInt32(i, c + 1);
                    questXpByLevel_[lvl] = row;
                }
                LOG_INFO("Loaded ", questXpByLevel_.size(), " quest XP levels");
            }
        }
    }
    auto it = questXpByLevel_.find(level);
    if (it == questXpByLevel_.end()) return 0;
    // The same column AzerothCore's Quest::XPValue reads: Exp[xpDifficulty].
    return it->second[xpDifficulty];
}

int32_t QuestHandler::getQuestRewardReputation(int32_t valueId, int32_t override) {
    // Returned in hundredths, the unit GetQuestLogRewardFactionInfo answers in
    // and questinfo.lua divides back down by 100. The override is already in
    // hundredths and wins when set - most WotLK quests use it. Otherwise the
    // dbc: row 1 for a gain, row 2 for a loss, column at the absolute value
    // index, a whole reputation scaled up to hundredths. Matches the display
    // value in Player::RewardReputation.
    if (override != 0) return override;
    if (valueId == 0) return 0;
    if (!questFactionRewDbcLoaded_) {
        questFactionRewDbcLoaded_ = true;
        auto* am = owner_.services().assetManager;
        if (am && am->isInitialized()) {
            auto dbc = am->loadDBC("QuestFactionReward.dbc");
            if (dbc && dbc->isLoaded() && dbc->getFieldCount() >= 11) {
                for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
                    const int32_t id = static_cast<int32_t>(dbc->getUInt32(i, 0));
                    std::array<int32_t, 10> row{};
                    for (uint32_t c = 0; c < 10; ++c)
                        row[c] = static_cast<int32_t>(dbc->getUInt32(i, c + 1));
                    questFactionRew_[id] = row;
                }
            }
        }
    }
    const int32_t rowId = (valueId < 0) ? 2 : 1;
    const uint32_t field = static_cast<uint32_t>(std::abs(valueId));
    if (field > 9) return 0;
    auto it = questFactionRew_.find(rowId);
    if (it == questFactionRew_.end()) return 0;
    return it->second[field] * 100;
}

bool QuestHandler::hasQuestInLog(uint32_t questId) const {
    for (const auto& q : questLog_) {
        if (q.questId == questId) return true;
    }
    return false;
}

std::vector<std::pair<uint32_t, uint32_t>> QuestHandler::getQuestTimers() const {
    std::vector<std::pair<uint32_t, uint32_t>> timers;
    if (owner_.lastPlayerFieldsRef().empty()) return timers;

    const uint16_t ufQuestStart = fieldIndex(UF::PLAYER_QUEST_LOG_START);
    const uint8_t qStride = owner_.getPacketParsers() ? owner_.getPacketParsers()->questLogStride() : 5;
    if (qStride == 0) return timers;
    const auto now = static_cast<uint64_t>(std::time(nullptr));
    const uint16_t maxSlots = static_cast<uint16_t>(maxQuestLogSlots());

    // Walked in quest log order rather than slot order: the timer frame numbers
    // its rows the way the log lists the quests, and the two disagree once a
    // quest has been turned in and its slot reused.
    for (const auto& q : questLog_) {
        if (q.questId == 0) continue;
        for (uint16_t slot = 0; slot < maxSlots; ++slot) {
            const uint16_t idField = ufQuestStart + slot * qStride;
            auto idIt = owner_.lastPlayerFieldsRef().find(idField);
            if (idIt == owner_.lastPlayerFieldsRef().end() || idIt->second != q.questId) continue;

            // The expiry is the last field of the slot in every expansion:
            // Classic packs state and counts together and so uses three, TBC
            // four, WotLK five.
            const uint16_t timeField = static_cast<uint16_t>(idField + qStride - 1);
            auto tIt = owner_.lastPlayerFieldsRef().find(timeField);
            if (tIt == owner_.lastPlayerFieldsRef().end() || tIt->second == 0) break;
            const auto expiry = static_cast<uint64_t>(tIt->second);
            // An expired timer is reported as zero rather than dropped, so the
            // row stays put until the server removes the quest.
            timers.emplace_back(q.questId,
                                expiry > now ? static_cast<uint32_t>(expiry - now) : 0u);
            break;
        }
    }
    return timers;
}

int QuestHandler::findQuestLogSlotIndexFromServer(uint32_t questId) const {
    if (questId == 0 || owner_.lastPlayerFieldsRef().empty()) return -1;
    const uint16_t ufQuestStart = fieldIndex(UF::PLAYER_QUEST_LOG_START);
    const uint8_t qStride = owner_.getPacketParsers() ? owner_.getPacketParsers()->questLogStride() : 5;
    const uint16_t maxSlots = static_cast<uint16_t>(maxQuestLogSlots());
    for (uint16_t slot = 0; slot < maxSlots; ++slot) {
        const uint16_t idField = ufQuestStart + slot * qStride;
        auto it = owner_.lastPlayerFieldsRef().find(idField);
        if (it != owner_.lastPlayerFieldsRef().end() && it->second == questId) {
            return static_cast<int>(slot);
        }
    }
    return -1;
}

void QuestHandler::addQuestToLocalLogIfMissing(uint32_t questId, const std::string& title, const std::string& objectives) {
    if (questId == 0 || hasQuestInLog(questId)) return;
    QuestLogEntry entry;
    entry.questId = questId;
    entry.title = title.empty() ? ("Quest #" + std::to_string(questId))
                                : normalizeQuestText(title, true);
    entry.objectives = normalizeQuestText(objectives, false);
    questLog_.push_back(std::move(entry));
    if (owner_.addonEventCallbackRef()) {
        owner_.addonEventCallbackRef()("QUEST_ACCEPTED", {std::to_string(questId)});
        owner_.addonEventCallbackRef()("QUEST_LOG_UPDATE", {});
        owner_.addonEventCallbackRef()("UNIT_QUEST_LOG_CHANGED", {"player"});
    }
}

bool QuestHandler::resyncQuestLogFromServerSlots(bool forceQueryMetadata) {
    if (owner_.lastPlayerFieldsRef().empty()) return false;

    const uint16_t ufQuestStart = fieldIndex(UF::PLAYER_QUEST_LOG_START);
    // Unmapped for this build's field table. applyQuestStateFromFields checks
    // this and this did not, so the slot arithmetic below ran from 0xFFFF and
    // every lookup missed - which reaches the same erase with an empty set.
    if (ufQuestStart == 0xFFFF) return false;
    const uint8_t qStride = owner_.getPacketParsers() ? owner_.getPacketParsers()->questLogStride() : 5;

    // Whether the quest area of the player's fields has arrived at all, which
    // is a different question from whether any player field has.
    //
    // This used to answer "done" on the first frame the field map was
    // non-empty, and the erase below then measured the local log against an
    // empty set and emptied it - a partial update block carrying a health or a
    // position change was enough. The resync runs once, so there was no second
    // chance: the quest log stayed empty for the session and the tracker with
    // it.
    //
    // Any key inside the quest slot range counts, including a zero one: a slot
    // written as zero is the server saying that slot is empty, which is real
    // information. Only the absence of every one of them means the block has
    // not come yet.
    const uint32_t questAreaEnd = static_cast<uint32_t>(ufQuestStart) + 25u * qStride;
    bool sawQuestArea = false;
    for (const auto& [key, value] : owner_.lastPlayerFieldsRef()) {
        (void)value;
        if (key >= ufQuestStart && key < questAreaEnd) { sawQuestArea = true; break; }
    }
    if (!sawQuestArea) return false;

    std::unordered_map<uint32_t, bool> serverQuestComplete;
    std::unordered_map<uint32_t, bool> serverQuestFailed;
    serverQuestComplete.reserve(25);
    serverQuestFailed.reserve(25);
    for (uint16_t slot = 0; slot < 25; ++slot) {
        const uint16_t idField    = ufQuestStart + slot * qStride;
        const uint16_t stateField = ufQuestStart + slot * qStride + 1;
        auto it = owner_.lastPlayerFieldsRef().find(idField);
        if (it == owner_.lastPlayerFieldsRef().end()) continue;
        uint32_t questId = it->second;
        if (questId == 0) continue;

        bool complete = false;
        bool failed = false;
        if (qStride >= 2) {
            auto stateIt = owner_.lastPlayerFieldsRef().find(stateField);
            if (stateIt != owner_.lastPlayerFieldsRef().end()) {
                complete = isQuestSlotComplete(qStride, stateIt->second);
                // The bit beside it, in the field already in hand.
                failed = isQuestSlotFailed(qStride, stateIt->second);
            }
        }
        serverQuestComplete[questId] = complete;
        serverQuestFailed[questId] = failed;
    }

    std::unordered_set<uint32_t> serverQuestIds;
    serverQuestIds.reserve(serverQuestComplete.size());
    for (const auto& [qid, _] : serverQuestComplete) serverQuestIds.insert(qid);

    const size_t localBefore = questLog_.size();
    std::erase_if(questLog_, [&](const QuestLogEntry& q) {
        return q.questId == 0 || serverQuestIds.count(q.questId) == 0;
    });
    const size_t removed = localBefore - questLog_.size();

    size_t added = 0;
    for (uint32_t questId : serverQuestIds) {
        if (hasQuestInLog(questId)) continue;
        addQuestToLocalLogIfMissing(questId, "Quest #" + std::to_string(questId), "");
        ++added;
    }

    size_t marked = 0;
    for (auto& quest : questLog_) {
        auto it = serverQuestComplete.find(quest.questId);
        if (it == serverQuestComplete.end()) continue;
        // Assigned, as the failed bit below: the slot is the server's answer,
        // and a quest stops being complete when it says so - a quest item
        // lost, or the quest dropped and taken again. Latched, the gossip and
        // greeting lists showed such a quest as ready to hand in.
        // (A layout with no state field says nothing either way. Absent from
        // the fields, the state is zero: the server sends no zero fields.)
        if (qStride >= 2 && it->second != quest.complete) {
            quest.complete = it->second;
            if (it->second) ++marked;
            LOG_DEBUG("Quest ", quest.questId, " complete=", it->second, " from update fields");
        }
        // Assigned rather than latched: a quest that stops being failed -
        // the server clears the bit when a timer is restarted - has to stop
        // reading as failed, and only the server knows when that happens.
        auto fit = serverQuestFailed.find(quest.questId);
        const bool nowFailed = (fit != serverQuestFailed.end()) && fit->second;
        if (nowFailed != quest.failed) {
            quest.failed = nowFailed;
            LOG_DEBUG("Quest ", quest.questId, " failed=", nowFailed,
                      " from update fields");
        }
    }

    if (forceQueryMetadata) {
        for (uint32_t questId : serverQuestIds) {
            requestQuestQuery(questId, false);
        }
    }

    LOG_INFO("Quest log resync from server slots: server=", serverQuestIds.size(),
             " localBefore=", localBefore, " removed=", removed, " added=", added,
             " markedComplete=", marked);
    return true;
}

void QuestHandler::applyQuestStateFromFields(const FlatFieldMap& fields) {
    const uint16_t ufQuestStart = fieldIndex(UF::PLAYER_QUEST_LOG_START);
    if (ufQuestStart == 0xFFFF) return;

    const uint8_t qStride = owner_.getPacketParsers() ? owner_.getPacketParsers()->questLogStride() : 5;
    if (qStride < 2) return;

    for (uint16_t slot = 0; slot < 25; ++slot) {
        const uint16_t idField    = ufQuestStart + slot * qStride;
        const uint16_t stateField = idField + 1;
        auto idIt = fields.find(idField);
        if (idIt == fields.end()) continue;
        uint32_t questId = idIt->second;
        if (questId == 0) continue;

        // Add quest to local log only if we have a pending accept for it
        if (!hasQuestInLog(questId) && pendingQuestAcceptTimeouts_.count(questId) != 0) {
            addQuestToLocalLogIfMissing(questId, "Quest #" + std::to_string(questId), "");
            requestQuestQuery(questId, false);
            // Re-query quest giver status for the NPC that gave us this quest
            auto pendingIt = pendingQuestAcceptNpcGuids_.find(questId);
            if (pendingIt != pendingQuestAcceptNpcGuids_.end() && pendingIt->second != 0 && owner_.getSocket()) {
                network::Packet qsPkt(wireOpcode(Opcode::CMSG_QUESTGIVER_STATUS_QUERY));
                qsPkt.writeUInt64(pendingIt->second);
                owner_.getSocket()->send(qsPkt);
            }
            clearPendingQuestAccept(questId);
        }

        // VALUES updates are authoritative quest progress too. Some servers do
        // not emit (or clients may miss) a separate ADD_KILL notification for
        // every counter change, so refresh the tracker from the quest slot.
        for (auto& quest : questLog_) {
            if (quest.questId == questId) {
                applyPackedKillCountsFromFields(quest);
                break;
            }
        }

        auto stateIt = fields.find(stateField);
        if (stateIt == fields.end()) continue;
        // The slot's state says complete or not, whichever it is: not only
        // on the way to complete (see resyncQuestLogFromServerSlots).
        const bool serverComplete = isQuestSlotComplete(qStride, stateIt->second);
        for (auto& quest : questLog_) {
            if (quest.questId == questId && quest.complete != serverComplete) {
                quest.complete = serverComplete;
                LOG_INFO("Quest ", questId, " complete=", serverComplete,
                         " from VALUES update field state");
                break;
            }
        }
    }
}

void QuestHandler::applyPackedKillCountsFromFields(QuestLogEntry& quest) {
    if (owner_.lastPlayerFieldsRef().empty()) return;

    const uint16_t ufQuestStart = fieldIndex(UF::PLAYER_QUEST_LOG_START);
    if (ufQuestStart == 0xFFFF) return;

    const uint8_t qStride = owner_.getPacketParsers() ? owner_.getPacketParsers()->questLogStride() : 5;
    if (qStride < 3) return;

    int slot = findQuestLogSlotIndexFromServer(quest.questId);
    if (slot < 0) return;

    const uint16_t countField1 = ufQuestStart + static_cast<uint16_t>(slot) * qStride
                               + questObjectiveCountFieldOffset(qStride);
    const uint16_t countField2 = (qStride >= 5)
                                     ? static_cast<uint16_t>(countField1 + 1)
                                     : static_cast<uint16_t>(0xFFFF);

    auto f1It = owner_.lastPlayerFieldsRef().find(countField1);
    if (f1It == owner_.lastPlayerFieldsRef().end()) return;
    const uint32_t packed1 = f1It->second;

    uint32_t packed2 = 0;
    if (countField2 != 0xFFFF) {
        auto f2It = owner_.lastPlayerFieldsRef().find(countField2);
        if (f2It != owner_.lastPlayerFieldsRef().end()) packed2 = f2It->second;
    }

    const auto counts = decodeQuestObjectiveCounts(qStride, packed1, packed2);

    // Apply kill objective counts (indices 0-3).
    for (int i = 0; i < 4; ++i) {
        const auto& obj = quest.killObjectives[i];
        if (obj.npcOrGoId == 0 || obj.required == 0) continue;
        const uint32_t entryKey = static_cast<uint32_t>(
            obj.npcOrGoId > 0 ? obj.npcOrGoId : -obj.npcOrGoId);
        if (counts[i] == 0 && quest.killCounts.count(entryKey)) continue;
        const uint32_t current = std::min(counts[i], obj.required);
        quest.killCounts[entryKey] = {current, obj.required};
        LOG_DEBUG("Quest ", quest.questId, " objective[", i, "]: npcOrGo=",
                  obj.npcOrGoId, " count=", current, "/", obj.required);
    }

    // Item collection progress is not stored in PLAYER_QUEST_LOG counters;
    // those fields contain the four creature/GO counters in every expansion.
    for (int i = 0; i < 6; ++i) {
        const auto& obj = quest.itemObjectives[i];
        if (obj.itemId == 0 || obj.required == 0) continue;
        quest.requiredItemCounts.emplace(obj.itemId, obj.required);
    }
}

void QuestHandler::clearPendingQuestAccept(uint32_t questId) {
    pendingQuestAcceptTimeouts_.erase(questId);
    pendingQuestAcceptNpcGuids_.erase(questId);
}

void QuestHandler::triggerQuestAcceptResync(uint32_t questId, uint64_t npcGuid, const char* reason) {
    if (questId == 0 || !owner_.getSocket() || owner_.getState() != WorldState::IN_WORLD) return;

    LOG_INFO("Quest accept resync: questId=", questId, " reason=", reason ? reason : "unknown");
    requestQuestQuery(questId, true);

    if (npcGuid != 0) {
        network::Packet qsPkt(wireOpcode(Opcode::CMSG_QUESTGIVER_STATUS_QUERY));
        qsPkt.writeUInt64(npcGuid);
        owner_.getSocket()->send(qsPkt);

        auto queryPkt = owner_.getPacketParsers()
            ? owner_.getPacketParsers()->buildQueryQuestPacket(npcGuid, questId)
            : QuestgiverQueryQuestPacket::build(npcGuid, questId);
        owner_.getSocket()->send(queryPkt);
    }
}

// ---------------------------------------------------------------------------
// Packet handlers
// ---------------------------------------------------------------------------

void QuestHandler::handleGossipMessage(network::Packet& packet) {
    const uint64_t gossipBefore = gossipWindowOpen_ ? currentGossip_.npcGuid : 0;
    bool ok = owner_.getPacketParsers() ? owner_.getPacketParsers()->parseGossipMessage(packet, currentGossip_)
                                    : GossipMessageParser::parse(packet, currentGossip_);
    if (!ok) return;
    if (questDetailsOpen_) return; // Don't reopen gossip while viewing quest
    gossipWindowOpen_ = true;
    // 0x0058a550: a gossip window with another object sets it.
    if (currentGossip_.npcGuid != gossipBefore) owner_.setInteractionObject(currentGossip_.npcGuid, go_interaction::Window::Gossip);
    // Gossip carries its text as an npc-text id, not inline, so any greeting
    // left over from a quest list belongs to a different window.
    questGreeting_.clear();

    // Asked for before the window is announced rather than after it.
    //
    // This client's own gossip window read the greeting again on every frame
    // it drew, so it collected the reply whenever it landed and the order here
    // could not matter. An interface driven by events reads it once, when it
    // is told the window opened - so announcing first meant it always read an
    // empty cache and drew the NPC dialog blank, and the text arriving a
    // moment later had nobody left to tell.
    //
    // Moving it earlier is enough on any NPC talked to twice, where the text
    // is already cached and the query is not sent at all. The first visit
    // still needs the arrival to say so, which handleNpcTextUpdate now does.
    if (currentGossip_.titleTextId > 0 && !npcTextCache_.count(currentGossip_.titleTextId)) {
        uint32_t wireOp = wireOpcode(Opcode::CMSG_NPC_TEXT_QUERY);
        if (wireOp != 0xFFFF && owner_.getSocket()) {
            network::Packet qPkt(static_cast<uint16_t>(wireOp));
            qPkt.writeUInt32(currentGossip_.titleTextId);
            qPkt.writeUInt64(currentGossip_.npcGuid);
            owner_.getSocket()->send(qPkt);
        }
    }

    if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("GOSSIP_SHOW", {});
    owner_.closeVendor(); // Close vendor if gossip opens

    // Classify gossip quests and update quest log + overhead NPC markers.
    classifyGossipQuests(true);

    // Play NPC greeting voice
    if (owner_.npcGreetingCallbackRef() && currentGossip_.npcGuid != 0) {
        auto entity = owner_.getEntityManager().getEntity(currentGossip_.npcGuid);
        if (entity) {
            glm::vec3 npcPos(entity->getX(), entity->getY(), entity->getZ());
            owner_.npcGreetingCallbackRef()(currentGossip_.npcGuid, npcPos);
        }
    }
}

void QuestHandler::handleQuestgiverQuestList(network::Packet& packet) {
    if (!packet.hasRemaining(8)) return;

    GossipMessageData data;
    data.npcGuid = packet.readUInt64();
    data.menuId = 0;
    data.titleTextId = 0;

    // What the quest giver says above its list. Read and discarded before, so
    // GetGreetingText had nothing to answer with and the greeting panel was
    // blank over a list that was otherwise correct.
    questGreeting_ = normalizeWowTextTokens(packet.readString());
    if (packet.hasRemaining(8)) {
        (void)packet.readUInt32(); // emoteDelay / unk
        (void)packet.readUInt32(); // emote / unk
    }

    // questCount is uint8 in all WoW versions for SMSG_QUESTGIVER_QUEST_LIST.
    uint32_t questCount = 0;
    if (packet.hasRemaining(1)) {
        questCount = packet.readUInt8();
    }

    const bool hasQuestFlagsField = !isClassicLikeExpansion() && !isActiveExpansion("tbc");

    data.quests.reserve(questCount);
    for (uint32_t i = 0; i < questCount; ++i) {
        if (!packet.hasRemaining(12)) break;
        GossipQuestItem q;
        q.questId = packet.readUInt32();
        q.questIcon = packet.readUInt32();
        q.questLevel = static_cast<int32_t>(packet.readUInt32());

        if (hasQuestFlagsField && packet.hasRemaining(5)) {
            q.questFlags = packet.readUInt32();
            q.isRepeatable = packet.readUInt8();
        } else {
            q.questFlags = 0;
            q.isRepeatable = 0;
        }
        q.title = normalizeWowTextTokens(packet.readString());
        if (q.questId != 0) {
            data.quests.push_back(std::move(q));
        }
    }

    currentGossip_ = std::move(data);
    gossipWindowOpen_ = true;
    // The quest frame's greeting (0x0058c5a0 with 0), which wowee keeps as
    // its gossip window.
    owner_.setQuestInteractionObject(currentGossip_.npcGuid, go_interaction::Window::Gossip);
    // QUEST_GREETING, not GOSSIP_SHOW. This is SMSG_QUESTGIVER_QUEST_LIST - a
    // quest giver listing what it has, which the greeting frame answers.
    // GOSSIP_SHOW belongs to SMSG_GOSSIP_MESSAGE and opens the gossip frame
    // instead, so firing it here put the wrong panel over the list. Both would
    // be worse: each frame shows itself on its own event, so sending both shows
    // two.
    if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("QUEST_GREETING", {});
    owner_.closeVendor();

    classifyGossipQuests(false);

    LOG_INFO("Questgiver quest list: npc=0x", std::hex, currentGossip_.npcGuid, std::dec,
             " quests=", currentGossip_.quests.size());
}

// Shared quest-icon classification for gossip windows. Derives NPC quest status
// from icon values so overhead markers stay aligned with what the NPC offers.
// updateQuestLog: if true, also patches quest log completion state (gossip handler
// does this because it has the freshest data; quest-list handler skips it because
// completion updates arrive via separate packets).
void QuestHandler::classifyGossipQuests(bool updateQuestLog) {
    // What each icon says, and what it cannot (gossip_quest_status.hpp).
    using namespace gossip_quest;
    auto isCompletable = [](uint32_t icon) { return iconIsCompletable(icon); };
    auto isIncomplete  = [](uint32_t icon) { return iconIsIncomplete(icon); };
    auto isAvailable   = [](uint32_t icon) { return iconIsAvailable(icon); };

    bool hasAvailable = false, hasReward = false, hasIncomplete = false;
    for (const auto& q : currentGossip_.quests) {
        bool completable = isCompletable(q.questIcon);
        bool incomplete  = isIncomplete(q.questIcon);
        bool available   = isAvailable(q.questIcon);
        hasAvailable |= available;
        hasReward |= completable;
        hasIncomplete |= incomplete;

        if (updateQuestLog) {
            for (auto& entry : questLog_) {
                if (entry.questId == q.questId) {
                    // Only an icon that says "done" is news: 4 comes for a
                    // finished quest and an unfinished one alike, and taking
                    // it as "not complete" cleared the slot's complete bit,
                    // so the list then showed every turn-in as unfinished.
                    entry.complete = logCompleteAfterList(q.questIcon, entry.complete);
                    entry.title = normalizeQuestText(q.title, true);
                    if (q.questLevel > 0 && entry.level <= 0) entry.level = q.questLevel;
                    break;
                }
            }
        }
    }
    if (currentGossip_.npcGuid != 0) {
        QuestGiverStatus status = QuestGiverStatus::NONE;
        if (hasReward) status = QuestGiverStatus::REWARD;
        else if (hasAvailable) status = QuestGiverStatus::AVAILABLE;
        else if (hasIncomplete) status = QuestGiverStatus::INCOMPLETE;

        // The overhead mark, but only where the server has not already said.
        //
        // SMSG_QUESTGIVER_STATUS carries a DIALOG_STATUS worked out for exactly
        // this question, and the client re-asks for it whenever a quest is
        // taken, advanced or handed in. What is derived here comes from the
        // per-quest icons in the list, which cannot say as much: the other
        // reader of that same field in this client - GetNumGossipAvailableQuests
        // - treats 4 as "active" and everything else as available, and a quest
        // that is finished and one that is not both arrive as active. Nothing
        // in the icons distinguishes them, so no list here can produce REWARD.
        //
        // Overwriting with that turned a turn-in's gold ? grey, or replaced it
        // with the ! of whatever else the NPC was offering - and it happened
        // only once the player had opened the window, which is why the mark
        // looked right until it was looked at.
        //
        // So this fills a gap rather than replacing an answer. Nothing and
        // NONE are both gaps; a real status is not.
        if (status != QuestGiverStatus::NONE) {
            const auto known = npcQuestStatus_.find(currentGossip_.npcGuid);
            if (known == npcQuestStatus_.end() ||
                known->second == QuestGiverStatus::NONE) {
                npcQuestStatus_[currentGossip_.npcGuid] = status;
            } else if (known->second != status) {
                // Once per questgiver, and only on a disagreement. The icon
                // values are the part still in doubt - two places in this
                // client read them differently - so this prints the ones that
                // actually arrived beside the status the server sent for the
                // same NPC. A run with no such line is a run where the two
                // agreed.
                static std::set<uint64_t> said;
                if (said.insert(currentGossip_.npcGuid).second) {
                    std::string icons;
                    for (const auto& q : currentGossip_.quests) {
                        if (!icons.empty()) icons += ",";
                        icons += std::to_string(q.questIcon);
                    }
                    LOG_WARNING("Questgiver 0x", std::hex, currentGossip_.npcGuid,
                                std::dec, ": server status ",
                                static_cast<int>(known->second),
                                ", quest icons (", icons, ") read as ",
                                static_cast<int>(status),
                                " - the server's is kept");
                }
            }
        }
    }
}

void QuestHandler::handleGossipComplete(network::Packet& packet) {
    (void)packet;

    // The creature's farewell is the interaction object's, said as it is let
    // go with the window (0x0058a550 -> 0x00518d50 -> 0x00720860).
    gossipWindowOpen_ = false;
    if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("GOSSIP_CLOSED", {});
    currentGossip_ = GossipMessageData{};
    questGreeting_.clear();
}

void QuestHandler::handleNpcTextUpdate(network::Packet& packet) {
    if (!packet.hasRemaining(4)) return;
    uint32_t textId = packet.readUInt32();
    // 8 text entries: each has probability(4) + text0 + text1 + lang(4) + 6 emote fields(24)
    for (int i = 0; i < 8; ++i) {
        if (!packet.hasRemaining(4)) break;
        /*float prob =*/ packet.readFloat();
        if (!packet.hasData()) break;
        std::string text0 = packet.readString();
        if (!packet.hasData()) break;
        std::string text1 = packet.readString();
        if (!packet.hasRemaining(28)) break; // lang(4) + 6×emote(4) = 28
        packet.setReadPos(packet.getReadPos() + 28);
        if (!text0.empty() && !npcTextCache_.count(textId)) {
            npcTextCache_[textId] = std::move(text0);
        } else if (!text1.empty() && !npcTextCache_.count(textId)) {
            npcTextCache_[textId] = std::move(text1);
        }
    }
    LOG_DEBUG("NPC text update: id=", textId,
             " cached=", npcTextCache_.count(textId) ? "yes" : "no");

    // The window is already open and waiting on exactly this text, so say so.
    //
    // A cache filled without an event is this client's oldest shape of bug and
    // it reads as "it only works the second time": the greeting is there for
    // every later visit to the same NPC and blank on the first, because the
    // first is the only one where the text arrives after the window did.
    // GOSSIP_SHOW is the event the interface redraws the greeting on, and
    // firing it again is what a second look at an open window means.
    if (gossipWindowOpen_ && currentGossip_.titleTextId == textId &&
        npcTextCache_.count(textId) && owner_.addonEventCallbackRef()) {
        owner_.addonEventCallbackRef()("GOSSIP_SHOW", {});
    }
}

const std::string& QuestHandler::getNpcText(uint32_t textId) const {
    static const std::string empty;
    auto it = npcTextCache_.find(textId);
    return (it != npcTextCache_.end()) ? it->second : empty;
}

void QuestHandler::handleQuestPoiQueryResponse(network::Packet& packet) {
    // WotLK 3.3.5a SMSG_QUEST_POI_QUERY_RESPONSE format:
    //   uint32 questCount
    //   per quest:
    //     uint32 questId
    //     uint32 poiCount
    //     per poi:
    //       uint32 poiId
    //       int32  objIndex      (-1 = no specific objective)
    //       uint32 mapId
    //       uint32 areaId
    //       uint32 floorId
    //       uint32 unk1
    //       uint32 unk2
    //       uint32 pointCount
    //       per point: int32 x, int32 y
    if (!packet.hasRemaining(4)) return;
    const uint32_t questCount = packet.readUInt32();
    for (uint32_t qi = 0; qi < questCount; ++qi) {
        if (!packet.hasRemaining(8)) return;
        const uint32_t questId  = packet.readUInt32();
        const uint32_t poiCount = packet.readUInt32();

        // Remove any previously added POI markers for this quest
        gossipPois_.erase(
            std::remove_if(gossipPois_.begin(), gossipPois_.end(),
                [questId](const GossipPoi& p) {
                    return p.data == questId;
                }),
            gossipPois_.end());

        // Every area, whatever its map, for the client's icon list
        // (0x005e7370); the markers below keep only this map's.
        std::vector<QuestPoiArea> iconAreas;
        bool complete = true;

        // Find the quest title for the marker label.
        std::string questTitle;
        for (const auto& q : questLog_) {
            if (q.questId == questId) { questTitle = q.title; break; }
        }

        for (uint32_t pi = 0; pi < poiCount; ++pi) {
            if (!packet.hasRemaining(32)) { complete = false; break; }
            QuestPoiArea area;
            area.poiId = packet.readUInt32();
            const int32_t objIndex = static_cast<int32_t>(packet.readUInt32());
            const uint32_t mapId    = packet.readUInt32();
            area.objectiveIndex = objIndex;
            area.mapId = mapId;
            area.worldMapAreaId = packet.readUInt32();
            area.floor = packet.readUInt32();
            area.priority = static_cast<int32_t>(packet.readUInt32());
            area.flags = packet.readUInt32();
            const uint32_t pointCount = packet.readUInt32();
            if (packet.getRemainingSize() < static_cast<size_t>(pointCount) * 8) {
                complete = false;
                break;
            }
            float sumX = 0.0f, sumY = 0.0f;
            std::vector<std::pair<float, float>> outline;
            outline.reserve(pointCount);
            area.points.reserve(pointCount);
            for (uint32_t pt = 0; pt < pointCount; ++pt) {
                const int32_t px = static_cast<int32_t>(packet.readUInt32());
                const int32_t py = static_cast<int32_t>(packet.readUInt32());
                area.points.emplace_back(px, py);
                sumX += static_cast<float>(px);
                sumY += static_cast<float>(py);
                // Canonical, the same swap the centroid below makes.
                outline.emplace_back(static_cast<float>(py), static_cast<float>(px));
            }
            iconAreas.push_back(std::move(area));
            if (pointCount == 0) continue;
            // Skip POIs for maps other than the player's current map.
            if (mapId != owner_.currentMapIdRef()) continue;
            GossipPoi poi;
            // QuestPOIPoint uses the opposite axis order from the canonical
            // world coordinates used by movement and the map renderer. Store
            // it canonically here so canonicalToRender() does not place every
            // objective far outside its zone bounds.
            poi.x    = sumY / static_cast<float>(pointCount);
            poi.y    = sumX / static_cast<float>(pointCount);
            poi.icon = 6;  // generic quest POI icon
            poi.data = questId;
            poi.questObjectiveIndex = objIndex;
            poi.name = questTitle.empty() ? "Quest objective" : questTitle;
            // Three points is the least that encloses anything. Fewer is a
            // line or a dot, which the centroid already says.
            if (outline.size() >= 3) poi.area = std::move(outline);
            LOG_DEBUG("Quest POI: questId=", questId, " mapId=", mapId,
                      " centroid=(", poi.x, ",", poi.y, ") title=", poi.name);
            if (gossipPois_.size() >= 200) gossipPois_.erase(gossipPois_.begin());
            gossipPois_.push_back(std::move(poi));
        }
        if (!complete) break;
        questPoiIcons_.receive(static_cast<int32_t>(questId), std::move(iconAreas),
                               [this](int32_t id) {
                                   return id > 0 && hasQuestInLog(static_cast<uint32_t>(id));
                               });
    }
    // The tracker draws its POI marks from this and had no way to know the
    // points had arrived - the query is answered well after the row is drawn.
    //
    // Fired for the tracker, not the map: this client owns the world map and
    // its POI layer, but the quest tracker is a separate frame and
    // QuestPOIGetIconInfo already answers it.
    if (owner_.addonEventCallbackRef()) {
        owner_.addonEventCallbackRef()("QUEST_POI_UPDATE", {});
    }
}

void QuestHandler::handleQuestDetails(network::Packet& packet) {
    QuestDetailsData data;
    bool ok = owner_.getPacketParsers() ? owner_.getPacketParsers()->parseQuestDetails(packet, data)
                                    : QuestDetailsParser::parse(packet, data);
    if (!ok) {
        LOG_WARNING("Failed to parse SMSG_QUESTGIVER_QUEST_DETAILS");
        return;
    }
    currentQuestDetails_ = data;
    questFrameFlags_ = data.questFlags;
    for (auto& q : questLog_) {
        if (q.questId != data.questId) continue;
        if (!data.title.empty() && (isPlaceholderQuestTitle(q.title) || data.title.size() >= q.title.size())) {
            q.title = normalizeQuestText(data.title, true);
        }
        if (!data.objectives.empty() && (q.objectives.empty() || data.objectives.size() > q.objectives.size())) {
            q.objectives = normalizeQuestText(data.objectives, false);
        }
        break;
    }
    // Pre-fetch item info for all reward items
    for (const auto& item : data.rewardChoiceItems) owner_.queryItemInfo(item.itemId, 0);
    for (const auto& item : data.rewardItems)       owner_.queryItemInfo(item.itemId, 0);
    // Open now, and let this client's own window wait for the item names.
    //
    // These two used to be one thing: the window was held shut for a hundred
    // milliseconds so the queries above could answer, and "open" was what the
    // interface's bindings read as well. See isQuestDetailsOpen.
    questDetailsOpen_ = true;
    questDetailsOpenTime_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    gossipWindowOpen_ = false;
    owner_.setQuestInteractionObject(currentQuestDetails_.npcGuid);
    if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("QUEST_DETAIL", {});
}

void QuestHandler::handleQuestRequestItems(network::Packet& packet) {
    QuestRequestItemsData data;
    if (!QuestRequestItemsParser::parse(packet, data)) {
        LOG_WARNING("Failed to parse SMSG_QUESTGIVER_REQUEST_ITEMS");
        return;
    }
    clearPendingQuestAccept(data.questId);

    if (pendingTurnInRewardRequest_ &&
        data.questId == pendingTurnInQuestId_ &&
        data.npcGuid == pendingTurnInNpcGuid_ &&
        data.isCompletable() &&
        owner_.getSocket()) {
        auto rewardReq = QuestgiverRequestRewardPacket::build(data.npcGuid, data.questId);
        owner_.getSocket()->send(rewardReq);
        pendingTurnInRewardRequest_ = false;
    }

    currentQuestRequestItems_ = data;
    questFrameFlags_ = data.questFlags;
    questRequestItemsOpen_ = true;
    gossipWindowOpen_ = false;
    questDetailsOpen_ = false;
    questDetailsOpenTime_ = std::chrono::steady_clock::time_point{};
    owner_.setQuestInteractionObject(data.npcGuid);

    // The panel shown when a quest is handed back before it is finished, or
    // when it asks for something in return. Its siblings QUEST_DETAIL and
    // QUEST_COMPLETE were both fired and this one was not, so the original
    // interface had the text for this step and no event to draw it on.
    if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("QUEST_PROGRESS", {});

    // Query item names for required items
    for (const auto& item : data.requiredItems) {
        owner_.queryItemInfo(item.itemId, 0);
    }

    // Server-authoritative turn-in requirements
    for (auto& q : questLog_) {
        if (q.questId != data.questId) continue;
        q.complete = data.isCompletable();
        q.requiredItemCounts.clear();

        std::ostringstream oss;
        if (!data.completionText.empty()) {
            oss << data.completionText;
            if (!data.requiredItems.empty() || data.requiredMoney > 0) oss << "\n\n";
        }
        if (!data.requiredItems.empty()) {
            oss << "Required items:";
            for (const auto& item : data.requiredItems) {
                std::string itemLabel = "Item " + std::to_string(item.itemId);
                if (const auto* info = owner_.getItemInfo(item.itemId)) {
                    if (!info->name.empty()) itemLabel = info->name;
                }
                q.requiredItemCounts[item.itemId] = item.count;
                oss << "\n- " << itemLabel << " x" << item.count;
            }
        }
        if (data.requiredMoney > 0) {
            if (!data.requiredItems.empty()) oss << "\n";
            oss << "\nRequired money: " << formatCopperAmount(data.requiredMoney);
        }
        q.objectives = oss.str();
        break;
    }
}

void QuestHandler::handleQuestOfferReward(network::Packet& packet) {
    QuestOfferRewardData data;
    if (!QuestOfferRewardParser::parse(packet, data)) {
        LOG_WARNING("Failed to parse SMSG_QUESTGIVER_OFFER_REWARD");
        return;
    }
    clearPendingQuestAccept(data.questId);
    LOG_INFO("Quest offer reward: questId=", data.questId, " title=\"", data.title, "\"");
    if (pendingTurnInQuestId_ == data.questId) {
        pendingTurnInQuestId_ = 0;
        pendingTurnInNpcGuid_ = 0;
        pendingTurnInRewardRequest_ = false;
    }
    currentQuestOfferReward_ = data;
    questFrameFlags_ = data.questFlags;
    questOfferRewardOpen_ = true;
    questRequestItemsOpen_ = false;
    gossipWindowOpen_ = false;
    questDetailsOpen_ = false;
    questDetailsOpenTime_ = std::chrono::steady_clock::time_point{};
    owner_.setQuestInteractionObject(data.npcGuid);
    if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("QUEST_COMPLETE", {});

    // Query item names for reward items
    for (const auto& item : data.choiceRewards)
        owner_.queryItemInfo(item.itemId, 0);
    for (const auto& item : data.fixedRewards)
        owner_.queryItemInfo(item.itemId, 0);
}

void QuestHandler::handleQuestConfirmAccept(network::Packet& packet) {
    size_t rem = packet.getRemainingSize();
    if (rem < 4) return;

    sharedQuestId_    = packet.readUInt32();
    sharedQuestTitle_ = packet.readString();
    if (packet.hasRemaining(8)) {
        sharedQuestSharerGuid_ = packet.readUInt64();
    }

    sharedQuestSharerName_.clear();
    auto entity = owner_.getEntityManager().getEntity(sharedQuestSharerGuid_);
    if (auto* unit = dynamic_cast<Unit*>(entity.get())) {
        sharedQuestSharerName_ = unit->getName();
    }
    if (sharedQuestSharerName_.empty()) {
        auto nit = owner_.getPlayerNameCache().find(sharedQuestSharerGuid_);
        if (nit != owner_.getPlayerNameCache().end())
            sharedQuestSharerName_ = nit->second;
    }
    if (sharedQuestSharerName_.empty()) {
        char tmp[32];
        std::snprintf(tmp, sizeof(tmp), "0x%llX",
                      static_cast<unsigned long long>(sharedQuestSharerGuid_));
        sharedQuestSharerName_ = tmp;
    }

    pendingSharedQuest_ = true;
    // No chat line: 0x006cbc50 -> 0x0058bc50 only raises QUEST_ACCEPT_CONFIRM,
    // whose popup says who and what. This printed an English line as well.
    // Who and what, in that order: uiparent.lua hands both straight to
    // StaticPopup_Show, whose text is "%s is starting %s". It picks between
    // two popups on whether the log is full, and answers the yes with
    // ConfirmAcceptQuest.
    owner_.fireAddonEvent("QUEST_ACCEPT_CONFIRM",
                          {sharedQuestSharerName_, sharedQuestTitle_});
    LOG_INFO("SMSG_QUEST_CONFIRM_ACCEPT: questId=", sharedQuestId_,
             " title=", sharedQuestTitle_, " sharer=", sharedQuestSharerName_);
}

} // namespace game
} // namespace wowee
