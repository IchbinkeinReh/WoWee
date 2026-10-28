// The dungeon finder's queue updates and proposals, read and decided the way
// wow.exe does it (0x0055bdc0, cases 0x361, 0x367 and 0x368). The packets are
// laid out as AzerothCore's WorldSession::SendLfgUpdatePlayer,
// SendLfgUpdateParty and SendLfgUpdateProposal write them.
//
// The raid browser's list rows are here too (SMSG_UPDATE_LFG_LIST 0x360, which
// the client reads at 0x0055b770); their layout comes out of that handler and
// the two field decoders it calls, not out of a core's writer.
#include <catch_amalgamated.hpp>

#include <cstdint>
#include <string>

#include "core/application.hpp"
#include "game/lfg_update.hpp"
#include "network/packet.hpp"

namespace wowee {
namespace core {
Application* Application::instance = nullptr;
}
}

using namespace wowee::game;
using wowee::network::Packet;

namespace {

const auto kNoRaid = [](const LfgUpdateBody&) { return false; };

/// SendLfgUpdatePlayer: type, extraInfo, then queued, two zero bytes, the
/// dungeons and the comment.
Packet playerUpdate(uint8_t type, bool queued, std::vector<uint32_t> dungeons) {
    Packet p(0x367);
    p.writeUInt8(type);
    p.writeUInt8(dungeons.empty() ? 0 : 1);
    if (!dungeons.empty()) {
        p.writeUInt8(queued ? 1 : 0);
        p.writeUInt8(0);
        p.writeUInt8(0);
        p.writeUInt8(static_cast<uint8_t>(dungeons.size()));
        for (uint32_t d : dungeons) p.writeUInt32(d);
        p.writeString("");
    }
    return p;
}

/// SendLfgUpdateParty: type, extraInfo, then joined, queued, two zero bytes,
/// three more, the dungeons and the comment.
Packet partyUpdate(uint8_t type, bool joined, bool queued, std::vector<uint32_t> dungeons) {
    Packet p(0x368);
    p.writeUInt8(type);
    const bool extra = joined || !dungeons.empty();
    p.writeUInt8(extra ? 1 : 0);
    if (extra) {
        p.writeUInt8(joined ? 1 : 0);
        p.writeUInt8(queued ? 1 : 0);
        p.writeUInt8(0); p.writeUInt8(0);
        for (int i = 0; i < 3; ++i) p.writeUInt8(0);
        p.writeUInt8(static_cast<uint8_t>(dungeons.size()));
        for (uint32_t d : dungeons) p.writeUInt32(d);
        p.writeString("");
    }
    return p;
}

constexpr uint32_t kRandom = 261u | (6u << 24);  // Random Wrath Heroic, type 6

/// The id pair a Looking For Raid list carries, repacked the way the client
/// repacks it (0x0055b770:9690).
constexpr uint32_t kLfrList = (6u << 24) | 1031u;

/// The fixed 256-byte comment field. The client reads the whole width
/// (0x0047b480(dst, 0x100)) whatever the terminator does, so the width is
/// written here even though the text stops early.
void writeComment(Packet& p, const std::string& text) {
    for (size_t i = 0; i < 0x100; ++i)
        p.writeUInt8(i < text.size() ? static_cast<uint8_t>(text[i]) : 0);
}

/// Mask bit 1 of a listing row: the byte the getters call a level, five more
/// bytes, then twenty uint32s (0x00559050) whose meaning was never shown.
void writeListingRun(Packet& p, uint8_t level) {
    p.writeUInt8(level);
    for (int i = 0; i < 5; ++i) p.writeUInt8(0);
    for (int i = 0; i < 20; ++i) p.writeUInt32(0);
}

}  // namespace

TEST_CASE("Both update bodies read to their end", "[lfg]") {
    uint8_t type = 0;
    LfgUpdateBody body;
    Packet p = playerUpdate(lfg_update_type::kJoinQueue, true, {kRandom});
    REQUIRE(parseLfgUpdate(p, false, type, body));
    CHECK(p.getRemainingSize() == 0);
    CHECK(type == lfg_update_type::kJoinQueue);
    CHECK(body.joined);
    CHECK(body.queued);
    REQUIRE(body.dungeons.size() == 1);
    CHECK(body.dungeons[0] == kRandom);

    Packet q = partyUpdate(lfg_update_type::kJoinQueue, true, false, {kRandom, 262u});
    REQUIRE(parseLfgUpdate(q, true, type, body));
    CHECK(q.getRemainingSize() == 0);
    CHECK(body.joined);
    CHECK_FALSE(body.queued);
    CHECK(body.dungeons.size() == 2);
}

TEST_CASE("The group found ends a proposal as succeeded, anything else as failed", "[lfg]") {
    uint8_t type = 0;
    LfgUpdateBody body, stored;
    // The proposal is accepted all round: the server takes the player out of
    // the queue with GROUP_FOUND (10) and no body.
    Packet p = playerUpdate(lfg_update_type::kGroupFound, false, {});
    REQUIRE(parseLfgUpdate(p, false, type, body));
    LfgUpdateOutcome o = decideLfgUpdate(type, body, stored, false, false, true, kNoRaid);
    CHECK(o.proposalEnds);
    CHECK_FALSE(o.proposalReturnedLine);
    CHECK(std::string(o.proposalEvent) == "LFG_PROPOSAL_SUCCEEDED");

    // Someone declined and the player is put back in the queue: still listed
    // and queued, the update type 8, and "returned to the queue".
    Packet q = playerUpdate(lfg_update_type::kProposalFailed, true, {kRandom});
    REQUIRE(parseLfgUpdate(q, false, type, body));
    o = decideLfgUpdate(type, body, stored, false, false, true, kNoRaid);
    CHECK(o.proposalEnds);
    CHECK(o.proposalReturnedLine);
    CHECK(std::string(o.proposalEvent) == "LFG_PROPOSAL_FAILED");

    // No proposal held, nothing to end.
    o = decideLfgUpdate(type, body, stored, false, false, false, kNoRaid);
    CHECK_FALSE(o.proposalEnds);
    CHECK(o.proposalEvent == nullptr);
}

TEST_CASE("Each update speaks only for its own side", "[lfg]") {
    LfgUpdateBody body, stored;
    // A player update while grouped: the proposal is the party packet's.
    LfgUpdateOutcome o = decideLfgUpdate(lfg_update_type::kGroupFound, body, stored,
                                         false, true, true, kNoRaid);
    CHECK_FALSE(o.proposalEnds);
    // The party one, while grouped, with 1 and 16 counted as success too.
    for (uint8_t t : {lfg_update_type::kGroupFound, lfg_update_type::kLeaderUnk1,
                      lfg_update_type::kGroupDisband}) {
        o = decideLfgUpdate(t, body, stored, true, true, true, kNoRaid);
        CHECK(o.proposalEnds);
        CHECK(std::string(o.proposalEvent) == "LFG_PROPOSAL_SUCCEEDED");
    }
    o = decideLfgUpdate(lfg_update_type::kRemovedFromQueue, body, stored, true, true, true, kNoRaid);
    CHECK(std::string(o.proposalEvent) == "LFG_PROPOSAL_FAILED");
}

TEST_CASE("Joining and leaving say so, and an unchanged queue says nothing", "[lfg]") {
    uint8_t type = 0;
    LfgUpdateBody none, joined;
    Packet p = playerUpdate(lfg_update_type::kJoinQueue, true, {kRandom});
    REQUIRE(parseLfgUpdate(p, false, type, joined));
    LfgUpdateOutcome o = decideLfgUpdate(type, joined, none, false, false, false, kNoRaid);
    CHECK(o.changed);
    CHECK(std::string(o.line) == "ERR_LFG_JOINED_QUEUE");
    CHECK(o.fireUpdate);

    // The routine restatement (14) of the same queue: nothing at all - no
    // line and no LFG_UPDATE.
    Packet same = playerUpdate(lfg_update_type::kUpdateStatus, true, {kRandom});
    LfgUpdateBody again;
    REQUIRE(parseLfgUpdate(same, false, type, again));
    o = decideLfgUpdate(type, again, joined, false, false, false, kNoRaid);
    CHECK_FALSE(o.changed);
    CHECK_FALSE(o.fireUpdate);

    // Removed from the queue (7): left. A raid in the list makes it the raid
    // browser's line.
    o = decideLfgUpdate(lfg_update_type::kRemovedFromQueue, none, joined, false, false, false, kNoRaid);
    CHECK(std::string(o.line) == "ERR_LFG_LEFT_QUEUE");
    o = decideLfgUpdate(lfg_update_type::kRemovedFromQueue, none, joined, false, false, false,
                        [](const LfgUpdateBody& b) { return b.extraInfo; });
    CHECK(std::string(o.line) == "ERR_LFG_LEFT_LIST");
    // Out for another reason (a failed role check, 6): no line.
    o = decideLfgUpdate(lfg_update_type::kRoleCheckFailed, none, joined, false, false, false, kNoRaid);
    CHECK(o.changed);
    CHECK(o.line == nullptr);
}

TEST_CASE("A party joined with no dungeon named is not announced", "[lfg]") {
    LfgUpdateBody stored, body;
    body.extraInfo = true;
    body.joined = true;
    LfgUpdateOutcome o = decideLfgUpdate(lfg_update_type::kJoinQueue, body, stored, true, true, false, kNoRaid);
    CHECK(o.changed);
    CHECK(o.line == nullptr);
    CHECK(o.fireUpdate);
}

namespace {

/// SendLfgUpdateProposal: dungeon, state, id, encounters, silent, count,
/// then roles, self, in dungeon, same group, answered, accepted per member.
Packet proposal(uint32_t id, bool silent, std::vector<LfgProposalRow> rows) {
    Packet p(0x361);
    p.writeUInt32(kRandom);
    p.writeUInt8(0);
    p.writeUInt32(id);
    p.writeUInt32(0);
    p.writeUInt8(silent ? 1 : 0);
    p.writeUInt8(static_cast<uint8_t>(rows.size()));
    for (const auto& r : rows) {
        p.writeUInt32(r.roles);
        p.writeUInt8(r.isSelf); p.writeUInt8(r.inDungeon); p.writeUInt8(r.sameGroup);
        p.writeUInt8(r.answered); p.writeUInt8(r.accepted);
    }
    return p;
}

}  // namespace

TEST_CASE("A proposal is shown once per id, and only when not silent", "[lfg]") {
    LfgProposalRow me{0x2, true, false, false, false, false};
    uint32_t lastId = 0;
    Packet p = proposal(5, true, {me});
    LfgProposal prop;
    REQUIRE(parseLfgProposal(p, prop));
    CHECK(p.getRemainingSize() == 0);
    // Silent: not shown, but the id is now seen ...
    LfgProposalOutcome o = decideLfgProposal(prop, lastId);
    CHECK_FALSE(o.show);
    CHECK(lastId == 5);
    // ... so the next update for it, not silent, does not show it either.
    Packet q = proposal(5, false, {me});
    REQUIRE(parseLfgProposal(q, prop));
    o = decideLfgProposal(prop, lastId);
    CHECK_FALSE(o.show);
    // A new id does.
    Packet r = proposal(6, false, {me});
    REQUIRE(parseLfgProposal(r, prop));
    o = decideLfgProposal(prop, lastId);
    CHECK(o.show);
    CHECK_FALSE(o.failed);
}

TEST_CASE("A decline fails the proposal, the party's before the player's", "[lfg]") {
    LfgProposalRow me{0x2, true, false, false, true, false};
    LfgProposalRow mate{0x4, false, false, true, true, false};
    LfgProposalRow stranger{0x8, false, false, false, true, false};
    uint32_t lastId = 0;
    LfgProposal prop;
    Packet p = proposal(7, false, {me, mate});
    REQUIRE(parseLfgProposal(p, prop));
    LfgProposalOutcome o = decideLfgProposal(prop, lastId);
    CHECK(o.failed);
    CHECK(std::string(o.declinedLine) == "ERR_LFG_PROPOSAL_DECLINED_PARTY");

    Packet q = proposal(7, false, {me, stranger});
    REQUIRE(parseLfgProposal(q, prop));
    o = decideLfgProposal(prop, lastId);
    CHECK(std::string(o.declinedLine) == "ERR_LFG_PROPOSAL_DECLINED_SELF");

    // A stranger declining is the server's to say, by the queue update.
    Packet r = proposal(7, false, {stranger});
    REQUIRE(parseLfgProposal(r, prop));
    o = decideLfgProposal(prop, lastId);
    CHECK_FALSE(o.failed);
}

TEST_CASE("Joining and leaving play their sounds with their lines", "[lfg]") {
    // 0x0055bdc0 plays PVPENTERQUEUE (8462) with the joined line and
    // LFG_Denied (17341) with the left one; no line, no sound.
    LfgUpdateBody none, joined;
    joined.extraInfo = joined.joined = joined.queued = true;
    joined.dungeons = {kRandom};
    LfgUpdateOutcome o = decideLfgUpdate(lfg_update_type::kJoinQueue, joined, none, false, false,
                                         false, kNoRaid);
    CHECK(o.sound == lfg_sound::kEnterQueue);
    o = decideLfgUpdate(lfg_update_type::kRemovedFromQueue, none, joined, false, false, false, kNoRaid);
    CHECK(o.sound == lfg_sound::kDenied);
    o = decideLfgUpdate(lfg_update_type::kRoleCheckFailed, none, joined, false, false, false, kNoRaid);
    CHECK(o.sound == 0);
}

TEST_CASE("A declined proposal plays LFG_Denied", "[lfg]") {
    LfgProposalRow me{0x2, true, false, false, true, false};
    LfgProposalRow waiting{0x2, true, false, false, false, false};
    uint32_t lastId = 0;
    LfgProposal prop;
    Packet p = proposal(8, false, {me});
    REQUIRE(parseLfgProposal(p, prop));
    CHECK(decideLfgProposal(prop, lastId).sound == lfg_sound::kDenied);
    Packet q = proposal(9, false, {waiting});
    REQUIRE(parseLfgProposal(q, prop));
    CHECK(decideLfgProposal(prop, lastId).sound == 0);
}

TEST_CASE("The party lock block reads member by member, in order", "[lfg]") {
    // LFGMgr's BuildPartyLockDungeonBlock: a byte count, then per member a
    // guid, a uint32 count and entry/status pairs - as SMSG_LFG_PARTY_INFO
    // and SMSG_LFG_JOIN_RESULT 6 carry it.
    Packet p(0x372);
    p.writeUInt8(2);
    p.writeUInt64(0x30);
    p.writeUInt32(2);
    p.writeUInt32(kRandom); p.writeUInt32(2);
    p.writeUInt32(206); p.writeUInt32(1);
    p.writeUInt64(0x20);
    p.writeUInt32(1);
    p.writeUInt32(574); p.writeUInt32(6);
    std::vector<LfgMemberLocks> party;
    REQUIRE(parseLfgPartyLocks(p, party));
    CHECK(p.getRemainingSize() == 0);
    REQUIRE(party.size() == 2);
    CHECK(party[0].guid == 0x30);
    REQUIRE(party[0].locks.size() == 2);
    CHECK(party[0].locks[0] == std::pair<uint32_t, uint32_t>{kRandom, 2});
    CHECK(party[0].locks[1] == std::pair<uint32_t, uint32_t>{206, 1});
    CHECK(party[1].guid == 0x20);
    CHECK(party[1].locks[0] == std::pair<uint32_t, uint32_t>{574, 6});

    Packet cut(0x372);
    cut.writeUInt8(1);
    cut.writeUInt64(0x30);
    cut.writeUInt32(2);
    cut.writeUInt32(206); cut.writeUInt32(1);
    CHECK_FALSE(parseLfgPartyLocks(cut, party));
    REQUIRE(party.size() == 1);
    CHECK(party[0].locks.size() == 1);
}

TEST_CASE("A whole raid browser list reads both record kinds", "[lfg]") {
    // 0x0055b770: the list id pair, a byte that says this is a whole list rather
    // than a delta, then the players (record 0x148, fields by 0x00553620) and
    // then the listings (0x1a8, fields by 0x00559050).
    Packet p(0x360);
    p.writeUInt32(6);
    p.writeUInt32(1031);
    p.writeUInt8(0);
    p.writeUInt32(1);              // one player row
    p.writeUInt32(7);              // and the uint32 after it: kept at 0x00beaaa4,
    p.writeUInt64(0x100);          //   written by the client and never read
    p.writeUInt32(0x2 | 0x10 | 0x80);
    writeComment(p, "need ranged");
    p.writeUInt8(1); p.writeUInt8(0); p.writeUInt8(1);  // +0x124..0x126
    p.writeUInt64(0x200);          // +0x128: eight bytes no getter reads
    p.writeUInt32(0b101);          // +0x130: the encounter mask
    p.writeUInt32(1);              // one listing row
    p.writeUInt32(9);              // ditto, at 0x00beaaa0
    p.writeUInt64(0x200);
    p.writeUInt32(0x1 | 0x2 | 0x8 | 0x40 | 0x80);
    writeListingRun(p, 80);
    writeComment(p, "ToC, 25 man");
    p.writeUInt64(0x100);          // +0x28: this listing's group, by guid
    p.writeUInt8(1);               // +0x88: the status the getter pushes 7th
    p.writeUInt64(0x100);          // +0x198: eight bytes no getter reads
    p.writeUInt32(0b11);           // +0x1a0: the encounter mask

    std::vector<LfgListRow> rows;
    REQUIRE(parseLfgList(p, kLfrList, rows));
    CHECK(p.getRemainingSize() == 0);
    REQUIRE(rows.size() == 2);     // arrival order: the players come first

    REQUIRE(rows[0].isListing == false);
    CHECK(rows[0].guid == 0x100);
    CHECK(rows[0].comment == "need ranged");
    CHECK(rows[0].hasRawBytes);
    CHECK(rows[0].rawBytes[0] == 1);
    CHECK(rows[0].rawBytes[2] == 1);
    CHECK(rows[0].ownerGuid == 0);  // a player row carries no group link
    CHECK(rows[0].encounterMask == 0b101);
    CHECK_FALSE(rows[0].hasLevel);  // only a listing row carries one

    REQUIRE(rows[1].isListing);
    CHECK(rows[1].guid == 0x200);
    CHECK(rows[1].hasLevel);
    CHECK(rows[1].level == 80);
    CHECK(rows[1].comment == "ToC, 25 man");
    CHECK(rows[1].listed);
    CHECK(rows[1].ownerGuid == 0x100);   // the listing belongs to that player row
    CHECK(rows[1].encounterMask == 0b11);
}

TEST_CASE("A list the client is not in is dropped, and its rows kept", "[lfg]") {
    std::vector<LfgListRow> rows;
    Packet whole(0x360);
    whole.writeUInt32(6);
    whole.writeUInt32(1031);
    whole.writeUInt8(0);
    whole.writeUInt32(1);
    whole.writeUInt32(0);
    whole.writeUInt64(0x100);
    whole.writeUInt32(0x2);
    writeComment(whole, "still here");
    whole.writeUInt32(0);
    whole.writeUInt32(0);
    REQUIRE(parseLfgList(whole, kLfrList, rows));
    CHECK(whole.getRemainingSize() == 0);
    REQUIRE(rows.size() == 1);

    // An update naming another list arrives after the client left it; the client
    // returns at 0x0055b770:9691-9694 without reading the rest of it.
    Packet other(0x360);
    other.writeUInt32(6);
    other.writeUInt32(1032);
    other.writeUInt8(1);
    other.writeUInt32(1);
    other.writeUInt64(0x100);      // a removal of the row that is held
    CHECK(parseLfgList(other, kLfrList, rows));
    CHECK(other.getRemainingSize() == 0);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].comment == "still here");
}

TEST_CASE("A delta drops the guids it names and fills the rows it repeats", "[lfg]") {
    std::vector<LfgListRow> rows;
    Packet whole(0x360);
    whole.writeUInt32(6);
    whole.writeUInt32(1031);
    whole.writeUInt8(0);
    whole.writeUInt32(2);          // two player rows: a plain one, and one whose
    whole.writeUInt32(0);          //   guid carries a flag bit of its own
    whole.writeUInt64(0x100);
    whole.writeUInt32(0x2 | 0x80);
    writeComment(whole, "10 man");
    whole.writeUInt64(0x200);
    whole.writeUInt32(0b101);
    whole.writeUInt64(0x0400000000000000ull);
    whole.writeUInt32(0);          // a row that carries no fields at all
    whole.writeUInt32(1);          // one listing row
    whole.writeUInt32(0);
    whole.writeUInt64(0x200);
    whole.writeUInt32(0x1 | 0x40);
    writeListingRun(whole, 80);
    whole.writeUInt8(1);
    REQUIRE(parseLfgList(whole, kLfrList, rows));
    CHECK(whole.getRemainingSize() == 0);
    REQUIRE(rows.size() == 3);

    Packet delta(0x360);
    delta.writeUInt32(6);
    delta.writeUInt32(1031);
    delta.writeUInt8(1);           // a delta: what is held stays held
    delta.writeUInt32(3);          // the block of guids the client names
    delta.writeUInt64(0x200);      // gone
    delta.writeUInt64(0);          // a zero guid is "touched", not removed
    delta.writeUInt64(0x0400000000000000ull);  // and a guid whose only set bit is
    delta.writeUInt32(1);          //   bit 26 of its high dword is not one either:
    delta.writeUInt32(0);          //   the test at 0x0055b770:9709 is 0xf07fffff,
    delta.writeUInt64(0x100);      //   and such a row is only unlinked from its
    delta.writeUInt32(0x2);        //   group, so it stays
    writeComment(delta, "25 man");
    delta.writeUInt32(0);
    delta.writeUInt32(0);
    REQUIRE(parseLfgList(delta, kLfrList, rows));
    CHECK(delta.getRemainingSize() == 0);

    // The row is updated where it is held rather than added again, and a mask
    // that leaves a field out leaves the value that was held for it.
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].guid == 0x100);
    CHECK(rows[0].comment == "25 man");
    CHECK(rows[0].encounterMask == 0b101);
    CHECK(rows[0].ownerGuid == 0);  // a player row's eight bytes were stepped over
    CHECK(rows[1].guid == 0x0400000000000000ull);
}

TEST_CASE("A cut short list keeps its header cleared and its rows half read", "[lfg]") {
    std::vector<LfgListRow> rows;
    Packet whole(0x360);
    whole.writeUInt32(6);
    whole.writeUInt32(1031);
    whole.writeUInt8(0);
    whole.writeUInt32(0);
    whole.writeUInt32(0);
    whole.writeUInt32(1);
    whole.writeUInt32(0);
    whole.writeUInt64(0x200);
    whole.writeUInt32(0x40);
    whole.writeUInt8(1);
    REQUIRE(parseLfgList(whole, kLfrList, rows));
    CHECK(whole.getRemainingSize() == 0);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].listed);

    // A whole list clears the window before it is read (the client clears it at
    // 0x00558af0), and each field read is guarded by the same bounds check the
    // client's readers make (0x0047b290): a packet that runs out stops here, with
    // the rows it got left in place - half read ones included, as the client's
    // record is allocated before its fields are stepped through.
    Packet cut(0x360);
    cut.writeUInt32(6);
    cut.writeUInt32(1031);
    cut.writeUInt8(0);
    cut.writeUInt32(2);
    cut.writeUInt32(0);
    cut.writeUInt64(0x100);
    cut.writeUInt32(0x2);
    writeComment(cut, "read whole");
    cut.writeUInt64(0x300);
    cut.writeUInt32(0x2);          // promised, never sent
    CHECK_FALSE(parseLfgList(cut, kLfrList, rows));
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].comment == "read whole");
    CHECK(rows[1].guid == 0x300);
    CHECK(rows[1].comment.empty());
    CHECK(rows[1].encounterMask == 0);
}
