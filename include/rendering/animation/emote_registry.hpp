#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace wowee {
namespace rendering {

// ============================================================================
// EmoteRegistry - extracted from AnimationController
//
// Owns all static emote data, DBC loading, emote text lookup, and
// animation ID resolution.  Singleton - loaded once on first use.
// ============================================================================

struct EmoteInfo {
    uint32_t animId = 0;
    uint32_t dbcId = 0;
    bool loop = false;
    std::string textNoTarget;
    std::string textTarget;
    std::string othersNoTarget;
    std::string othersTarget;
    std::string command;
};

class EmoteRegistry {
public:
    static EmoteRegistry& instance();

    /// Load emotes from DBC files (called once on first use).
    void loadFromDbc();

    struct EmoteResult { uint32_t animId; bool loop; };

    /// Look up an emote by chat command (e.g. "dance", "wave").
    [[nodiscard]] std::optional<EmoteResult> findEmote(const std::string& command) const;


    /// Get the animation ID for an Emotes.dbc emote ID, as used by SMSG_EMOTE
    /// and UNIT_NPC_EMOTESTATE.
    [[nodiscard]] uint32_t animByEmotesId(uint32_t emoteId) const;
    /// Emotes.dbc EmoteFlags (+0xc) of a row; 0 without one.
    [[nodiscard]] uint32_t flagsByEmotesId(uint32_t emoteId) const;

    /// The Emotes.dbc row a chat line's speaker gestures with (0x007165d0,
    /// table 0x00ca1224): kind 0 talk (EmoteFlags 0x8), 1 question (0x10),
    /// 2 exclamation (0x20), 3 shout (0x40), 4 laugh (0x100) - the first row
    /// with that flag, each row counted for its first flag of the five only.
    /// 0 for none.
    [[nodiscard]] uint32_t chatGestureEmote(int kind) const;
    /// The same choice over rows given as (id, flags) in file order.
    static std::array<uint32_t, 5> chatGestureRows(const std::vector<std::pair<uint32_t, uint32_t>>& rows);
    /// True if the Emotes.dbc entry is a persistent state emote (EmoteSpecProc
    /// != 0, e.g. STATE_WORK_NOSHEATHE) rather than a one-shot (ONESHOT_WAVE).
    [[nodiscard]] bool isStateEmote(uint32_t emoteId) const;


    /// Get first-person emote text for a command.
    std::string textFor(const std::string& emoteName,
                        const std::string* targetName = nullptr) const;

    /// Get DBC ID for an emote command.
    [[nodiscard]] uint32_t dbcIdFor(const std::string& emoteName) const;

    /// Get third-person emote text by DBC ID.
    std::string textByDbcId(uint32_t dbcId,
                            const std::string& senderName,
                            const std::string* targetName = nullptr) const;

    /// The first-person text by DBC ID - "You wave." - for the server's echo
    /// of the player's own emote, which is the line the chat shows for it.
    std::string selfTextByDbcId(uint32_t dbcId,
                                const std::string* targetName = nullptr) const;


private:
    EmoteRegistry() = default;
    EmoteRegistry(const EmoteRegistry&) = delete;
    EmoteRegistry& operator=(const EmoteRegistry&) = delete;

    void loadFallbackEmotes();
    void buildDbcIdIndex();

    bool loaded_ = false;
    std::unordered_map<std::string, EmoteInfo> emoteTable_;
    std::unordered_map<uint32_t, const EmoteInfo*> emoteByDbcId_;
    std::unordered_map<uint32_t, uint32_t> animByEmotesId_;
    std::unordered_map<uint32_t, uint32_t> flagsByEmotesId_;
    std::unordered_set<uint32_t> stateEmoteIds_;
    std::array<uint32_t, 5> chatGestureEmotes_{};
};

} // namespace rendering
} // namespace wowee
