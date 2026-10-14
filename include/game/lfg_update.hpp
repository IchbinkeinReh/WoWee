#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace wowee {
namespace network { class Packet; }
namespace game {

/**
 * SMSG_LFG_UPDATE_PLAYER and SMSG_LFG_UPDATE_PARTY, and what wow.exe does
 * with them (0x0055bdc0, cases 0x367 and 0x368).
 *
 * The update types are numbered as the client reads them. The ones it tests
 * are 1, 7, 10, 15 and 16, and they fit AzerothCore's LfgUpdateType - group
 * found at 10, removed from the queue at 7, a member offline at 15.
 */
namespace lfg_update_type {
constexpr uint8_t kDefault            = 0;
constexpr uint8_t kLeaderUnk1         = 1;
constexpr uint8_t kRoleCheckAborted   = 4;
constexpr uint8_t kJoinQueue          = 5;
constexpr uint8_t kRoleCheckFailed    = 6;
constexpr uint8_t kRemovedFromQueue   = 7;
constexpr uint8_t kProposalFailed     = 8;
constexpr uint8_t kProposalDeclined   = 9;
constexpr uint8_t kGroupFound         = 10;
constexpr uint8_t kAddedToQueue       = 12;
constexpr uint8_t kProposalBegin      = 13;
constexpr uint8_t kUpdateStatus       = 14;
constexpr uint8_t kGroupMemberOffline = 15;
constexpr uint8_t kGroupDisband       = 16;
}  // namespace lfg_update_type

/// The body of either update, as the client keeps it to compare with the
/// next one (0x00acd9a0 for the player, 0x00acdac0 for the party).
struct LfgUpdateBody {
    bool extraInfo = false;
    bool joined = false;      ///< the player packet's extraInfo; the party's own byte
    bool queued = false;
    bool unk1 = false;
    bool unk2 = false;
    uint8_t partyBytes[3] = {};
    std::vector<uint32_t> dungeons;   ///< entries, the type in the top byte
    std::string comment;

    /// 0x005570f0: the five flags, the party bytes, the count, the comment
    /// and every dungeon of one found in the other.
    [[nodiscard]] bool sameAs(const LfgUpdateBody& o) const;
};

/// Read either update. False when it is cut short.
bool parseLfgUpdate(network::Packet& packet, bool party, uint8_t& updateType,
                    LfgUpdateBody& body);

/// What one update does, in the order the client does it.
struct LfgUpdateOutcome {
    /// The proposal ends here: drop it, then the line and the event below.
    bool proposalEnds = false;
    bool proposalReturnedLine = false;      ///< ERR_LFG_PROPOSAL_FAILED
    const char* proposalEvent = nullptr;    ///< LFG_PROPOSAL_SUCCEEDED / _FAILED
    /// The queue as stated moved; store the new body.
    bool changed = false;
    /// The interface string to say, or none.
    const char* line = nullptr;
    /// LFG_UPDATE, false.
    bool fireUpdate = false;
};

/// Decide what 0x0055bdc0 does with an update. `hasRaid` answers whether a
/// body names a raid, which makes the lines the raid browser's
/// (0x00557260).
LfgUpdateOutcome decideLfgUpdate(uint8_t updateType, const LfgUpdateBody& body,
                                 const LfgUpdateBody& stored, bool party, bool grouped,
                                 bool proposalValid,
                                 const std::function<bool(const LfgUpdateBody&)>& hasRaid);

/// One member of a proposed group, as SMSG_LFG_PROPOSAL_UPDATE lists them.
struct LfgProposalRow {
    uint32_t roles = 0;
    bool isSelf = false;
    bool inDungeon = false;
    bool sameGroup = false;
    bool answered = false;
    bool accepted = false;
};

/// SMSG_LFG_PROPOSAL_UPDATE as 0x0055bdc0 (case 0x361) reads it.
struct LfgProposal {
    uint32_t dungeonEntry = 0;
    uint8_t state = 0;
    uint32_t id = 0;
    uint32_t encountersDone = 0;
    bool silent = false;
    std::vector<LfgProposalRow> members;   ///< five at most, as the client keeps
};

bool parseLfgProposal(network::Packet& packet, LfgProposal& out);

/// What the proposal update raises, in order.
struct LfgProposalOutcome {
    bool show = false;                 ///< LFG_PROPOSAL_SHOW, after LFG_PROPOSAL_UPDATE
    const char* declinedLine = nullptr;  ///< ERR_LFG_PROPOSAL_DECLINED_PARTY / _SELF
    bool failed = false;               ///< the proposal dropped and LFG_PROPOSAL_FAILED
};

/// `lastId` is the last proposal id seen, which this updates (0x00552900).
LfgProposalOutcome decideLfgProposal(const LfgProposal& p, uint32_t& lastId);

}  // namespace game
}  // namespace wowee
