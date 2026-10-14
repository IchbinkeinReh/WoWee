#include "game/lfg_update.hpp"

#include <algorithm>

#include "network/packet.hpp"

namespace wowee {
namespace game {

bool LfgUpdateBody::sameAs(const LfgUpdateBody& o) const {
    if (extraInfo != o.extraInfo || joined != o.joined || queued != o.queued ||
        unk1 != o.unk1 || unk2 != o.unk2) return false;
    if (partyBytes[0] != o.partyBytes[0] || partyBytes[1] != o.partyBytes[1] ||
        partyBytes[2] != o.partyBytes[2]) return false;
    if (dungeons.size() != o.dungeons.size() || comment != o.comment) return false;
    for (uint32_t d : dungeons)
        if (std::find(o.dungeons.begin(), o.dungeons.end(), d) == o.dungeons.end()) return false;
    return true;
}

bool parseLfgUpdate(network::Packet& packet, bool party, uint8_t& updateType,
                    LfgUpdateBody& body) {
    // Player:  type, extraInfo, [queued, unk, unk, count, dungeons, comment]
    // Party:   type, extraInfo, [joined, queued, unk, unk, 3 bytes, count,
    //                            dungeons, comment]
    body = LfgUpdateBody{};
    if (!packet.hasRemaining(2)) return false;
    updateType = packet.readUInt8();
    body.extraInfo = packet.readUInt8() != 0;
    if (!party) body.joined = body.extraInfo;
    if (!body.extraInfo) return true;
    if (party) {
        if (!packet.hasRemaining(7)) return false;
        body.joined = packet.readUInt8() != 0;
        body.queued = packet.readUInt8() != 0;
        body.unk1 = packet.readUInt8() != 0;
        body.unk2 = packet.readUInt8() != 0;
        for (uint8_t& b : body.partyBytes) b = packet.readUInt8();
    } else {
        if (!packet.hasRemaining(3)) return false;
        body.queued = packet.readUInt8() != 0;
        body.unk1 = packet.readUInt8() != 0;
        body.unk2 = packet.readUInt8() != 0;
    }
    if (!packet.hasRemaining(1)) return false;
    const uint8_t count = packet.readUInt8();
    for (uint8_t i = 0; i < count; ++i) {
        if (!packet.hasRemaining(4)) return false;
        body.dungeons.push_back(packet.readUInt32());
    }
    if (packet.hasData()) body.comment = packet.readString();
    return true;
}

LfgUpdateOutcome decideLfgUpdate(uint8_t updateType, const LfgUpdateBody& body,
                                 const LfgUpdateBody& stored, bool party, bool grouped,
                                 bool proposalValid,
                                 const std::function<bool(const LfgUpdateBody&)>& hasRaid) {
    using namespace lfg_update_type;
    LfgUpdateOutcome out;
    // The player packet speaks for a player on their own and the party one
    // for a group; each acts only on its own side.
    const bool applies = party ? grouped : !grouped;

    // A proposal the update ends - the queue left, or still queued - is
    // dropped, and this is where LFG_PROPOSAL_SUCCEEDED or _FAILED come from:
    // succeeded for the group found, and for the party also update types 1
    // and 16; failed for anything else. Still listed, it is
    // ERR_LFG_PROPOSAL_FAILED - back in the queue.
    if (proposalValid && (!body.extraInfo || body.queued) && applies) {
        out.proposalEnds = true;
        out.proposalReturnedLine = body.extraInfo;
        const bool succeeded = party ? (updateType == kGroupFound || updateType == kLeaderUnk1 ||
                                        updateType == kGroupDisband)
                                     : updateType == kGroupFound;
        out.proposalEvent = succeeded ? "LFG_PROPOSAL_SUCCEEDED" : "LFG_PROPOSAL_FAILED";
    }

    // Nothing more unless the queue as stated has moved.
    if (body.sameAs(stored)) return out;
    out.changed = true;

    // Joining (the raid browser's list, for a raid) is ERR_LFG_JOINED_QUEUE /
    // _LIST - for the party only once it names a dungeon; leaving, no longer
    // joined or no longer queued, ERR_LFG_LEFT_QUEUE / _LIST and only for
    // update types 7 and 15.
    const auto left = [&]() -> const char* {
        if (updateType != kRemovedFromQueue && updateType != kGroupMemberOffline) return nullptr;
        return hasRaid(stored) ? "ERR_LFG_LEFT_LIST" : "ERR_LFG_LEFT_QUEUE";
    };
    const auto leftQueue = [&]() -> const char* {
        return (!body.queued && stored.queued) ? left() : nullptr;
    };
    if (!body.joined) {
        out.line = stored.joined ? left() : leftQueue();
    } else if (!stored.joined && (!party || !body.dungeons.empty())) {
        out.line = hasRaid(body) ? "ERR_LFG_JOINED_LIST" : "ERR_LFG_JOINED_QUEUE";
    } else {
        out.line = leftQueue();
    }
    out.fireUpdate = applies;
    return out;
}

bool parseLfgProposal(network::Packet& packet, LfgProposal& out) {
    out = LfgProposal{};
    if (!packet.hasRemaining(4 + 1 + 4 + 4 + 1 + 1)) return false;
    out.dungeonEntry = packet.readUInt32();
    out.state = packet.readUInt8();
    out.id = packet.readUInt32();
    out.encountersDone = packet.readUInt32();
    out.silent = packet.readUInt8() != 0;
    const uint8_t count = packet.readUInt8();
    for (uint8_t i = 0; i < count; ++i) {
        // Five at most: the client stops filling its 0x78-byte table there,
        // and reads nothing past it.
        if (i >= 5) break;
        if (!packet.hasRemaining(4 + 5)) return false;
        LfgProposalRow r;
        r.roles = packet.readUInt32();
        r.isSelf = packet.readUInt8() != 0;
        r.inDungeon = packet.readUInt8() != 0;
        r.sameGroup = packet.readUInt8() != 0;
        r.answered = packet.readUInt8() != 0;
        r.accepted = packet.readUInt8() != 0;
        out.members.push_back(r);
    }
    return true;
}

LfgProposalOutcome decideLfgProposal(const LfgProposal& p, uint32_t& lastId) {
    LfgProposalOutcome out;
    // A new id is taken as seen whether or not it is shown, so a silent
    // offer is not shown by its next update.
    if (p.id != lastId) {
        lastId = p.id;
        out.show = !p.silent;
    }
    // Someone of the player's own group turning it down first, then the
    // player; either ends it.
    bool self = false, partyMember = false;
    for (const auto& m : p.members) {
        if (!m.answered || m.accepted) continue;
        if (!m.isSelf) {
            if (m.sameGroup) partyMember = true;
        } else {
            self = true;
        }
    }
    if (partyMember) out.declinedLine = "ERR_LFG_PROPOSAL_DECLINED_PARTY";
    else if (self) out.declinedLine = "ERR_LFG_PROPOSAL_DECLINED_SELF";
    out.failed = out.declinedLine != nullptr;
    return out;
}

}  // namespace game
}  // namespace wowee
