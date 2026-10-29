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
    // Whether the packet was a whole list is the caller's question to ask: the client
    // clears its window then, which is also where it unregisters the name callbacks
    // its rows registered (0x00558af0:6967).
    bool wholeList = false;
    REQUIRE(parseLfgList(whole, kLfrList, rows, &wholeList));
    CHECK(wholeList);
    CHECK(whole.getRemainingSize() == 0);
    REQUIRE(rows.size() == 1);

    // An update naming another list arrives after the client left it; the client
    // returns at 0x0055b770:9691-9694 without reading the rest of it — so none of it
    // is a whole list, because none of it is read.
    Packet other(0x360);
    other.writeUInt32(6);
    other.writeUInt32(1032);
    other.writeUInt8(1);
    other.writeUInt32(1);
    other.writeUInt64(0x100);      // a removal of the row that is held
    bool matched = true;
    CHECK(parseLfgList(other, kLfrList, rows, &wholeList, &matched));
    CHECK_FALSE(wholeList);
    CHECK_FALSE(matched);
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
    delta.writeUInt32(1);          //   bit 26 of its high dword fails the test at
    delta.writeUInt32(0);          //   0x0055b770:9709 (0xf07fffff): it names a group,
    delta.writeUInt64(0x100);      //   which 0x005589d0 deletes with its members
    delta.writeUInt32(0x2);
    writeComment(delta, "25 man");
    delta.writeUInt32(0);
    delta.writeUInt32(0);
    REQUIRE(parseLfgList(delta, kLfrList, rows));
    CHECK(delta.getRemainingSize() == 0);

    // The row is updated where it is held rather than added again, and a mask
    // that leaves a field out leaves the value that was held for it. The group
    // named by the failing guid is gone.
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].guid == 0x100);
    CHECK(rows[0].comment == "25 man");
    CHECK(rows[0].encounterMask == 0b101);
    CHECK(rows[0].ownerGuid == 0);  // a player row's eight bytes were stepped over
}

TEST_CASE("A removed group takes the listings it owns with it", "[lfg]") {
    std::vector<LfgListRow> rows;
    LfgListRow group;
    group.guid = 0x1F50000000000042ull;
    rows.push_back(group);
    LfgListRow member;
    member.guid = 0x300;
    member.isListing = true;
    member.ownerGuid = group.guid;
    rows.push_back(member);
    LfgListRow loner;
    loner.guid = 0x301;
    loner.isListing = true;
    rows.push_back(loner);

    Packet delta(0x360);
    delta.writeUInt32(6);
    delta.writeUInt32(1031);
    delta.writeUInt8(1);
    delta.writeUInt32(1);
    delta.writeUInt64(group.guid);  // top nibble set: a group (0x005589d0)
    delta.writeUInt32(0);
    delta.writeUInt32(0);
    delta.writeUInt32(0);
    delta.writeUInt32(0);
    REQUIRE(parseLfgList(delta, kLfrList, rows));
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].guid == 0x301);
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

TEST_CASE("Rows are paired into the rows the browse panel shows", "[lfg]") {
    // 0x00553530 resolves an index to one of two arrays: the listings that belong to
    // no one, then the player records and their member[0] — which of the two comes
    // first is the byte at 0x00acd660, and the LFG module's init sets it
    // (0x0055d440:10772), so the listings come first from the start.
    std::vector<LfgListRow> rows;
    auto add = [&](uint64_t guid, bool listing, uint64_t owner = 0) {
        LfgListRow r;
        r.guid = guid;
        r.isListing = listing;
        r.ownerGuid = owner;
        rows.push_back(r);
        return static_cast<uint32_t>(rows.size() - 1);
    };
    const uint32_t group = add(0x100, false);            // a group's player record
    const uint32_t own = add(0x200, true, 0x100);        // its first listing
    const uint32_t mate = add(0x201, true, 0x100);       // and another member
    add(0x300, false);                                  // a player with nothing listed
    const uint32_t solo = add(0x400, true);              // a listing in no group
    const uint32_t orphan = add(0x500, true, 0x999);     // one whose group never came
    const uint32_t atListing = add(0x600, true, 0x200);  // one naming a listing, which

    const auto shown = buildLfgBrowserRows(rows, true);
    REQUIRE(shown.size() == 4);                          //   has nowhere to attach

    CHECK(shown[0].listing == solo);                     // the array of listings that
    CHECK(shown[0].player == kLfgNoRow);                 //   belong to no one comes
    CHECK(shown[1].listing == orphan);                   //   first, in the order the
    CHECK(shown[2].listing == atListing);                //   packet brought them

    CHECK(shown[3].player == group);                     // then a group's row, whose
    CHECK(shown[3].listing == own);                      // listing is member[0] itself
    REQUIRE(shown[3].members.size() == 1);
    CHECK(shown[3].members[0] == mate);                  // party results start at [1]

    // Clearing that byte walks the player array first instead (0x00553530:2831),
    // which is what a "group" header click flips to.
    const auto swapped = buildLfgBrowserRows(rows, false);
    REQUIRE(swapped.size() == 4);
    CHECK(swapped[0].player == group);
    CHECK(swapped[1].listing == solo);
    CHECK(swapped[2].listing == orphan);
    CHECK(swapped[3].listing == atListing);

    // A player record with no listing attached is no row at all: 0x00553530 hands
    // back a null listing for it and 0x00555fd0 returns no values, so the panel
    // never sees the index and never SetTexts a nil.
    rows.clear();
    add(0x100, false);
    CHECK(buildLfgBrowserRows(rows, true).empty());
}

TEST_CASE("A column header makes its key primary, and a second click reverses it", "[lfg]") {
    auto keys = defaultLfgSortKeys();
    CHECK(keys.size() == lfg_sort_key::kCount);
    CHECK(keys[0].key == lfg_sort_key::kZone);           // the init's order, 0x0055d440
    CHECK_FALSE(keys[0].descending);                     //   and none of them reversed

    // The words the browse panel's headers carry (lfrframe.xml:697-805), read without
    // regard to case (0x0076e780 is strnicmp), and a word the client does not name
    // falls to key 3 — the same key "name" gives (0x0055d2b0:10643).
    CHECK(lfgSortKeyForName("level")  == lfg_sort_key::kLevel);
    CHECK(lfgSortKeyForName("Healer") == lfg_sort_key::kHealer);
    CHECK(lfgSortKeyForName("class")  == lfg_sort_key::kClass);
    CHECK(lfgSortKeyForName("zone")   == lfg_sort_key::kZone);
    CHECK(lfgSortKeyForName("name")   == lfg_sort_key::kName);
    CHECK(lfgSortKeyForName("nonsense") == lfg_sort_key::kName);

    // The key moves to slot 0 keeping its direction, the others shifting down one
    // (:10691-10699). "tank" is key 4, three slots ahead of the level already at the
    // front once the zone key says nothing.
    applyLfgSortKey(keys, lfg_sort_key::kLevel);
    CHECK(keys[0].key == lfg_sort_key::kLevel);
    CHECK(keys[1].key == lfg_sort_key::kZone);
    CHECK(keys[2].key == lfg_sort_key::kClass);
    applyLfgSortKey(keys, lfg_sort_key::kTank);
    CHECK(keys[0].key == lfg_sort_key::kTank);
    CHECK(keys[1].key == lfg_sort_key::kLevel);
    CHECK(keys[2].key == lfg_sort_key::kZone);

    // A second click on the same header is the only thing that reverses it (:10688).
    applyLfgSortKey(keys, lfg_sort_key::kTank);
    CHECK(keys[0].key == lfg_sort_key::kTank);
    CHECK(keys[0].descending);
    CHECK_FALSE(keys[1].descending);
    applyLfgSortKey(keys, lfg_sort_key::kTank);
    CHECK_FALSE(keys[0].descending);

    // "group" never reaches the key list at all: the client tests it first and swaps
    // the two arrays of rows instead, sorting nothing (:10645, :10709-10711).
    auto before = keys;
    bool listingsFirst = true;
    CHECK(applyLfgSortHeaderClick(keys, listingsFirst, "GROUP"));
    CHECK_FALSE(listingsFirst);
    CHECK(keys == before);                               //   the list untouched
    CHECK_FALSE(applyLfgSortHeaderClick(keys, listingsFirst, "level"));
    CHECK_FALSE(listingsFirst);                          //   the swap untouched
    CHECK(keys[0].key == lfg_sort_key::kLevel);
    CHECK_FALSE(applyLfgSortHeaderClick(keys, listingsFirst, "tank"));
    CHECK(keys[0].key == lfg_sort_key::kTank);
}

TEST_CASE("The two arrays are sorted by their own comparators", "[lfg]") {
    // Three listings belonging to no one and one group's row, with the level at
    // listing +0x2c and the role bits at +0x90 the keys read.
    std::vector<LfgListRow> rows;
    auto add = [&](uint64_t guid, bool listing, uint64_t owner, uint8_t level, uint8_t roles) {
        LfgListRow r;
        r.guid = guid;
        r.isListing = listing;
        r.ownerGuid = owner;
        r.level = level;
        r.hasLevel = true;
        r.roles = roles;
        rows.push_back(r);
        return static_cast<uint32_t>(rows.size() - 1);
    };
    const uint32_t leader = add(0x100, false, 0, 80, 0);      // a group's player record
    const uint32_t head   = add(0x200, true, 0x100, 70, 0x2); // its member[0]
    add(0x300, true, 0, 85, 0x8);                             // damage, no group
    add(0x400, true, 0, 85, 0x4);                             // healer, no group
    add(0x500, true, 0, 81, 0);                               // and one by level alone

    auto shown = buildLfgBrowserRows(rows, true);
    REQUIRE(shown.size() == 4);
    CHECK(shown[0].listing == 2);        // arrival order until the sort runs
    CHECK(shown[3].player == leader);

    auto keys = defaultLfgSortKeys();
    applyLfgSortKey(keys, lfg_sort_key::kLevel);
    const auto nameOf = [](const LfgListRow&) { return std::string{}; };

    // The two arrays, sorted separately: the array of listings by all seven keys and
    // the group row by the player comparator, which handles four of them at most
    // (0x00555a70's switch stops at case 3, 0x00555cd0's runs to case 6).
    sortLfgBrowserRows(shown, rows, keys, nameOf);
    CHECK(shown[0].listing == 4);        //   level 81 first, ascending
    CHECK(shown[1].listing == 3);        //   the two level-85 rows then go to the
    CHECK(shown[2].listing == 2);        //   healer key still in the list
    CHECK(shown[3].player == leader);    //   the group row keeps its own array's place

    // The role keys decide nothing for a group's row and everything for the other
    // array: sorting by healer moves the healer listing to the front.
    applyLfgSortKey(keys, lfg_sort_key::kHealer);
    sortLfgBrowserRows(shown, rows, keys, nameOf);
    CHECK(shown[0].listing == 3);        //   a row that has the bit comes first
    CHECK(shown[1].listing == 4);        //   (:4741-4752 subtracts the far row's bit)
    CHECK(shown[2].listing == 2);
    CHECK(shown[3].player == leader);

    // A second click on that header reverses only it, and the rows without the bit
    // fall back on the level key behind it.
    applyLfgSortKey(keys, lfg_sort_key::kHealer);
    sortLfgBrowserRows(shown, rows, keys, nameOf);
    CHECK(shown[0].listing == 4);
    CHECK(shown[1].listing == 2);
    CHECK(shown[2].listing == 3);

    // Two rows read by each comparator, with nothing else in the key list. The role
    // key is what tells them apart for the listings and says nothing at all for a
    // player row.
    const std::vector<LfgSortEntry> roleOnly{LfgSortEntry{lfg_sort_key::kHealer, false}};
    LfgBrowserRow withBit, withoutBit;
    withBit.listing = 3;
    withoutBit.listing = 4;
    CHECK(compareLfgBrowserRows(withBit, withoutBit, rows, roleOnly, false, nameOf) < 0);
    CHECK(compareLfgBrowserRows(withoutBit, withBit, rows, roleOnly, false, nameOf) > 0);
    CHECK(compareLfgBrowserRows(withBit, withoutBit, rows, roleOnly, true, nameOf) == 0);

    // The name key, compared as strncmp of whatever the guids resolve to; nothing
    // resolving says nothing, which is the client's own answer for an empty lookup
    // (:4727-4740).
    const std::vector<LfgSortEntry> nameKey{LfgSortEntry{lfg_sort_key::kName, false}};
    CHECK(compareLfgBrowserRows(withoutBit, withBit, rows, nameKey, false, nameOf) == 0);
    // strncmp, not stricmp: an upper case letter sorts before a lower case one, which
    // is what 0x0076e760 is and not what the header words' compare is.
    const auto named = [](const LfgListRow& r) {
        return std::string(r.guid == 0x400 ? "Zara" : r.guid == 0x500 ? "arthur" : "");
    };
    CHECK(compareLfgBrowserRows(withBit, withoutBit, rows, nameKey, false, named) < 0);
}

TEST_CASE("The last name the list asked for is the one that re-announces it", "[lfg]") {
    // 0x005528a0, read off the binary — Ghidra left those 48 bytes as a label, and the
    // two sites that register it pass no guid to it: it closes one request off a count
    // and fires 469 only when that count runs out.
    uint32_t outstanding = 2;
    CHECK_FALSE(lfgNameAnswerClosesGate(outstanding));
    CHECK(outstanding == 1);
    CHECK(lfgNameAnswerClosesGate(outstanding));
    CHECK(outstanding == 0);
    // Nothing outstanding: an answer no row is waiting on says nothing, and the count
    // does not run below zero into a gate that closes again on some later answer.
    CHECK_FALSE(lfgNameAnswerClosesGate(outstanding));
    CHECK(outstanding == 0);
}

TEST_CASE("What the browser raises is the event its id names, not the queue's", "[lfg]") {
    // The client keeps the name of every script event in a table indexed by the event id:
    // FUN_00608880 (0x00608880) writes one name per id up from 0x00c24eb0, and FUN_0081ac90
    // looks an event up in the registry built from it by the same arithmetic
    // (DAT_00d3f7d8 + id*4). So the id the browser's six sites fire — 469 — is the name at
    // 0x00c24eb0 + 469*4 = 0x00c25604, which the same function writes as "UPDATE_LFG_LIST",
    // between "UPDATE_LFG_TYPES" (468) and "UPDATE_LFG_LIST_INCREMENTAL" (470) — the last
    // two never fired anywhere in this client.
    CHECK(0x00c24eb0u + 469u * 4u == 0x00c25604u);
    CHECK(kLfgListChangedEvent == "UPDATE_LFG_LIST");
    // LFG_UPDATE is event 501, a different one, and the LFG module fires it with a boolean
    // ("%b" at 0x00a02e38 — true from 0x00559310, false from :10509 and :10619). The name
    // matters because lfrframe.lua's handlers differ: UPDATE_LFG_LIST refills the browse
    // list (:31-34), LFG_UPDATE only the comment box (:35-55).
    CHECK(kLfgListChangedEvent != "LFG_UPDATE");
}

TEST_CASE("A row's bosses are the joined instance's rows of the encounter table", "[lfg]") {
    // 0x00553830 walks the whole table counting the rows of one (map, difficulty)
    // pair and among them those whose bit the mask holds; 0x005538b0 walks the same
    // table counting *down* to the encounterIndex-th. The rows below are laid out
    // as the file stores them, which is why Shadowfang's two difficulties sit apart
    // and the two Naxxramas rows share a pair.
    const std::vector<LfgEncounterRow> rows = {
        {33, 0, 0, "Rotkralle"},
        {33, 0, 1, "Klingenklaue der Metzger"},
        {33, 1, 0, "Anub'Rekhan"},
        {43, 0, 0, "Dochivle"},
        {533, 1, 0, "Grofwitwe Faerlina"},
        {533, 1, 32, "eine Zeile, die Bit 32 nennt"},
    };

    const auto none = countLfgEncounters(rows, 33, 0, 0);
    CHECK(none.total == 2);                     //   the pair, not the map
    CHECK(none.complete == 0);

    const auto one = countLfgEncounters(rows, 33, 0, 0b10);
    CHECK(one.total == 2);
    CHECK(one.complete == 1);                   //   bit 1 of the mask

    // A mask naming a boss this instance does not list adds nothing: the two
    // numbers come out of the same walk, which is why the interface's "all bosses
    // alive" line is reachable at all (lfrframe.lua:676 asks for a total above zero
    // and a complete of zero).
    CHECK(countLfgEncounters(rows, 33, 0, 0b100).complete == 0);
    CHECK(countLfgEncounters(rows, 43, 0, 0b1).complete == 1);
    CHECK(countLfgEncounters(rows, 616, 0, 0xFFFFFFFFu).total == 0);

    // The client shifts by the byte masked to five bits (0x00553830:3021), so a row
    // naming bit 32 addresses bit 0 rather than nothing.
    const auto masked = countLfgEncounters(rows, 533, 1, 0b1);
    CHECK(masked.total == 2);
    CHECK(masked.complete == 2);

    // The nth boss of one instance, in the order the table stores them.
    CHECK(lfgEncounterAt(rows, 33, 0, 0)->name == "Rotkralle");
    CHECK(lfgEncounterAt(rows, 33, 0, 1)->name == "Klingenklaue der Metzger");
    CHECK(lfgEncounterAt(rows, 33, 0, 2) == nullptr);      //   past the end: three nils
    CHECK(lfgEncounterAt(rows, 33, 1, 0)->name == "Anub'Rekhan");
    CHECK(lfgEncounterAt(rows, 0, 0, 0) == nullptr);

    CHECK(lfgEncounterDone(0b1, rows[0]));                 // the mask, one bit per boss
    CHECK_FALSE(lfgEncounterDone(0b10, rows[0]));
    CHECK(lfgEncounterDone(0b1, rows[5]));                 // bit 32 asks as bit 0 does
}
