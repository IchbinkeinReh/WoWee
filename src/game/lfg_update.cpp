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
    //
    // Each line comes with its sound: PVPENTERQUEUE joining, LFG_Denied
    // leaving.
    if (!body.joined) {
        out.line = stored.joined ? left() : leftQueue();
    } else if (!stored.joined && (!party || !body.dungeons.empty())) {
        out.line = hasRaid(body) ? "ERR_LFG_JOINED_LIST" : "ERR_LFG_JOINED_QUEUE";
        out.sound = lfg_sound::kEnterQueue;
    } else {
        out.line = leftQueue();
    }
    if (out.line && !out.sound) out.sound = lfg_sound::kDenied;
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
    if (out.failed) out.sound = lfg_sound::kDenied;
    return out;
}

bool parseLfgPartyLocks(network::Packet& packet, std::vector<LfgMemberLocks>& out) {
    out.clear();
    if (!packet.hasRemaining(1)) return false;
    const uint8_t members = packet.readUInt8();
    for (uint8_t i = 0; i < members; ++i) {
        if (!packet.hasRemaining(8 + 4)) return false;
        const uint64_t guid = packet.readUInt64();
        // Found or added at the end (0x0055aea0).
        auto member = std::find_if(out.begin(), out.end(),
                                   [guid](const LfgMemberLocks& m) { return m.guid == guid; });
        if (member == out.end()) {
            out.push_back({guid, {}});
            member = out.end() - 1;
        }
        const uint32_t count = packet.readUInt32();
        for (uint32_t j = 0; j < count; ++j) {
            if (!packet.hasRemaining(8)) return false;
            const uint32_t entry = packet.readUInt32();
            const uint32_t status = packet.readUInt32();
            auto lock = std::find_if(member->locks.begin(), member->locks.end(),
                                     [entry](const auto& l) { return l.first == entry; });
            if (lock == member->locks.end()) member->locks.emplace_back(entry, status);
            else lock->second = status;
        }
    }
    return true;
}

bool parseLfgList(network::Packet& packet, uint32_t heldListId,
                  std::vector<LfgListRow>& rows) {
    if (!packet.hasRemaining(4 + 4 + 1)) return false;
    const uint32_t typeId = packet.readUInt32();
    const uint32_t listId = (typeId << 24) | (packet.readUInt32() & 0x00FFFFFFu);
    const bool deltas = packet.readUInt8() != 0;
    if (listId != heldListId) {
        // Not the list this client is in. The client returns without reading the
        // rest (0x0055b770:9691-9694) — an update for a list already left.
        packet.skipAll();
        return true;
    }
    // The byte being clear is what makes the packet a whole list rather than a
    // delta: the client clears its window (0x00558af0) and reads no removals.
    if (!deltas) rows.clear();

    // Every field goes through this, so a packet that runs out is noticed once and
    // the rows already read are kept — the same bounds check the client's readers
    // make (0x0047b290) before each of theirs.
    struct Reader {
        network::Packet& p;
        bool ok = true;
        uint8_t u8() {
            if (!ok || !p.hasRemaining(1)) { ok = false; return 0; }
            return p.readUInt8();
        }
        uint32_t u32() {
            if (!ok || !p.hasRemaining(4)) { ok = false; return 0; }
            return p.readUInt32();
        }
        uint64_t u64() {
            if (!ok || !p.hasRemaining(8)) { ok = false; return 0; }
            return p.readUInt64();
        }
        /// A fixed-width string field: the client reads the whole width
        /// (0x0047b480(dst, 0x100)) whatever the terminator does, so the width is
        /// consumed even where the text stops early.
        std::string fixed(size_t width) {
            std::string s;
            for (size_t i = 0; i < width; ++i) {
                const char c = static_cast<char>(u8());
                if (c != '\0') s.push_back(c);
            }
            return s;
        }
    } in{packet};

    auto findRow = [&](uint64_t g, bool listing) -> LfgListRow* {
        for (auto& r : rows)
            if (r.guid == g && r.isListing == listing) return &r;
        return nullptr;
    };
    // Rows are keyed by their guid, as the client's guid hash keys its records
    // (0x006792e0): a row that arrives again updates the one held rather than
    // adding a second.
    auto rowFor = [&](uint64_t g, bool listing) -> LfgListRow& {
        if (LfgListRow* found = findRow(g, listing)) return *found;
        rows.push_back(LfgListRow{});
        LfgListRow& r = rows.back();
        r.guid = g;
        r.isListing = listing;
        return r;
    };

    if (deltas) {
        const uint32_t count = in.u32();
        for (uint32_t i = 0; i < count && in.ok; ++i) {
            const uint64_t g = in.u64();
            // This block carries nothing but the guid, and the test the client
            // makes on it (0x0055b770:9709) is a guid test, not a field mask: the
            // top nibble of the high dword clear, and either the low dword set or
            // a bit set below it — 0xf07fffff, which leaves out bits 23 to 27. An
            // entry passing it is a row deleted; one failing it names a row that
            // is only unlinked from its group (0x005589d0 → 0x00556f90), and since
            // no group membership is held here that half does nothing yet.
            const uint32_t low = static_cast<uint32_t>(g);
            const uint32_t high = static_cast<uint32_t>(g >> 32);
            const bool removal = (high & 0xf0000000u) == 0 &&
                                 (low != 0 || (high & 0xf07fffffu) != 0);
            if (removal) std::erase_if(rows, [&](const LfgListRow& r) { return r.guid == g; });
        }
    }

    const auto readFields = [&](LfgListRow& row, bool listing) {
        const uint32_t mask = in.u32();
        if (!listing) {
            // 0x00553620: the comment, three bytes the getters have no shown
            // meaning for, and the encounter mask at +0x130 — whose leading eight
            // bytes at +0x128 no getter ever reads, so they are stepped over.
            if (mask & 0x2) row.comment = in.fixed(0x100);
            if (mask & 0x10) {
                for (uint8_t& b : row.rawBytes) b = in.u8();
                row.hasRawBytes = true;
            }
            if (mask & 0x80) {
                in.u64();
                row.encounterMask = in.u32();
            }
            return;
        }
        // 0x00559050. Bit 1 is a run of six bytes then twenty uint32s, at record
        // +0x2c through +0x80; the getters show a meaning for exactly one of
        // those, the byte at +0x2c, which SearchLFGGetResults pushes as the row's
        // level, so only that is kept and the rest is stepped over.
        if (mask & 0x1) {
            row.level = in.u8();
            row.hasLevel = true;
            for (int k = 0; k < 5; ++k) in.u8();
            for (int k = 0; k < 20; ++k) in.u32();
        }
        if (mask & 0x2) row.comment = in.fixed(0x100);
        if (mask & 0x4) {
            // +0x8c. The reader compares the old value to the new (0x00559050:
            // 7338-7347): a flip for a row that already has a group detaches it
            // and re-inserts it, and the insert puts a set byte at the head of
            // the member array (0x00556e60). That is where a row's place in its
            // group comes from; nothing here orders groups, so the byte itself
            // is stepped over.
            in.u8();
        }
        if (mask & 0x8) {
            // The row's group, resolved to a record by the guid hash and stored
            // at +0x28 (0x00559050:7348-7383): the row is then a member of it.
            row.ownerGuid = in.u64();
        }
        // +0x90 and +0x84, and the eight bytes ahead of the encounter mask at
        // +0x1a0: read by this decoder, read by no getter.
        if (mask & 0x10) in.u8();
        if (mask & 0x20) in.u32();
        if (mask & 0x40) row.listed = in.u8() != 0;
        if (mask & 0x80) {
            in.u64();
            row.encounterMask = in.u32();
        }
    };

    // Two counted blocks, the players first and then the listings, which is the
    // order the client's row getter walks them in unless the byte at 0x00acd660
    // says otherwise; nothing here applies that swap. Each block's count is
    // followed by one more uint32 — the client keeps it at 0x00beaaa4 and
    // 0x00beaaa0 (0x0055b770:9733, 9847), and grepping every read of those two
    // globals in the whole decompile turns up nothing: written, never read. It
    // still occupies its four bytes on the wire, so it goes here.
    const uint32_t playerCount = in.u32();
    in.u32();
    for (uint32_t i = 0; i < playerCount && in.ok; ++i) {
        const uint64_t g = in.u64();
        readFields(rowFor(g, false), false);
    }
    const uint32_t listingCount = in.u32();
    in.u32();
    for (uint32_t i = 0; i < listingCount && in.ok; ++i) {
        const uint64_t g = in.u64();
        readFields(rowFor(g, true), true);
    }
    return in.ok;
}

std::vector<LfgBrowserRow> buildLfgBrowserRows(const std::vector<LfgListRow>& rows) {
    // Which player record each listing names as its group. The client resolves the
    // same guid through its record hash and stores what answers at listing +0x28
    // (0x00559050:7348-7383); nothing answering is what puts a listing in the
    // array of listings belonging to no one (0x0055b770:9940-9974). A guid naming
    // a *listing* rather than a player record is treated the same way: the member
    // array lives in the player record's shape (+0x138, +0x13c), so a listing has
    // nowhere to attach and the row getter would never reach it.
    std::vector<uint32_t> owner(rows.size(), kLfgNoRow);
    for (uint32_t k = 0; k < rows.size(); ++k) {
        if (!rows[k].isListing || rows[k].ownerGuid == 0) continue;
        for (uint32_t i = 0; i < rows.size(); ++i)
            if (!rows[i].isListing && rows[i].guid == rows[k].ownerGuid) { owner[k] = i; break; }
    }

    std::vector<LfgBrowserRow> out;
    for (uint32_t i = 0; i < rows.size(); ++i) {
        if (rows[i].isListing) continue;
        LfgBrowserRow row;
        row.player = i;
        for (uint32_t k = 0; k < rows.size(); ++k)
            if (owner[k] == i) row.members.push_back(k);
        // The row's own listing is the group's first member, and the getter hands
        // back a null listing for a player with none - which the row getter turns
        // into no values at all (0x00555fd0:4850-4852), so there is no such row.
        if (row.members.empty()) continue;
        row.listing = row.members.front();
        row.members.erase(row.members.begin());
        out.push_back(std::move(row));
    }
    for (uint32_t k = 0; k < rows.size(); ++k) {
        if (!rows[k].isListing || owner[k] != kLfgNoRow) continue;
        LfgBrowserRow row;
        row.listing = k;
        out.push_back(std::move(row));
    }
    return out;
}

LfgEncounterCounts countLfgEncounters(const std::vector<LfgEncounterRow>& rows,
                                      uint32_t mapId, uint32_t difficulty,
                                      uint32_t mask) {
    // One pass over the whole table, counting the rows of one instance and among
    // them those whose bit the mask has (0x00553830). Both numbers come out of the
    // same walk, which is why a boss the mask names but the table does not has no
    // count of its own.
    LfgEncounterCounts out;
    for (const auto& row : rows) {
        if (row.mapId != mapId || row.difficulty != difficulty) continue;
        ++out.total;
        if (lfgEncounterDone(mask, row)) ++out.complete;
    }
    return out;
}

const LfgEncounterRow* lfgEncounterAt(const std::vector<LfgEncounterRow>& rows,
                                      uint32_t mapId, uint32_t difficulty,
                                      uint32_t before) {
    // 0x005538b0 walks the same table and counts *down*: every row of the instance
    // it passes decrements the index, and the row that reaches zero is the answer.
    // Order is the table's own, which is id order in the file, so the bosses of one
    // instance come out in the order they are fought.
    for (const auto& row : rows) {
        if (row.mapId != mapId || row.difficulty != difficulty) continue;
        if (before == 0) return &row;
        --before;
    }
    return nullptr;
}

}  // namespace game
}  // namespace wowee
