// The game errors a handful of server packets come down to (server_notices.hpp):
// which row of the client's table at 0x00ac8af0 each answer is, and which line
// 0x005216f0 puts it on.
#include <catch_amalgamated.hpp>

#include <string>

#include "game/server_notices.hpp"

namespace sn = wowee::game::server_notice;

TEST_CASE("MSG_QUEST_PUSH_RESULT 0..11 are rows 0x19b..0x1a6, all in chat", "[notices]") {
    for (uint8_t r = 0; r < 12; ++r) {
        const auto* e = sn::questPushResult(r);
        REQUIRE(e != nullptr);
        CHECK(e->index == 0x19b + r);
        CHECK(e->line == sn::Line::Chat);
    }
    CHECK(std::string(sn::questPushResult(0)->key) == "ERR_QUEST_PUSH_SUCCESS_S");
    CHECK(std::string(sn::questPushResult(3)->key) == "ERR_QUEST_PUSH_DECLINED_S");
    CHECK(std::string(sn::questPushResult(11)->key) == "ERR_QUEST_PUSH_DIFFERENT_SERVER_DAILY_S");
    // The switch has no case past 11: nothing is said.
    for (uint8_t quiet : {uint8_t{12}, uint8_t{13}, uint8_t{255}}) CHECK(sn::questPushResult(quiet) == nullptr);
}

TEST_CASE("a voice taken away or given back, to the player and to the leader", "[notices]") {
    CHECK(sn::partySquelch(0).index == 0x241);
    CHECK(sn::partySquelch(1).index == 0x240);
    // Any word but zero is "taken away".
    CHECK(sn::partySquelch(7).index == 0x240);
    CHECK(sn::partySquelchEcho(0).index == 0x250);
    CHECK(sn::partySquelchEcho(1).index == 0x24f);
    CHECK(std::string(sn::partySquelchEcho(1).key) == "ERR_PLAYER_SILENCED_ECHO");
}

TEST_CASE("the single-error opcodes go where their rows say", "[notices]") {
    CHECK(sn::kComsatReconnectAttempt.line == sn::Line::Chat);
    CHECK(sn::kVoiceSessionFull.index == 0x251);
    CHECK(sn::kBattlegroundInfoThrottled.line == sn::Line::Chat);
    CHECK(sn::kInvalidPromotionCode.line == sn::Line::Error);
    CHECK(sn::kArenaTeamChangeFailedQueued.line == sn::Line::Error);
    CHECK(sn::kGmResponseDbError.index == 0x2a1);
}

TEST_CASE("SMSG_GM_TICKET_STATUS_UPDATE through its jump table", "[notices]") {
    using A = sn::TicketStatusAction;
    CHECK(sn::ticketStatusAction(1) == A::Refresh);
    CHECK(sn::ticketStatusAction(2) == A::Closed);
    CHECK(sn::ticketStatusAction(3) == A::ClosedSurvey);
    CHECK(sn::ticketStatusAction(4) == A::Refresh);
    CHECK(sn::ticketStatusAction(0) == A::None);
    CHECK(sn::ticketStatusAction(5) == A::None);
}

TEST_CASE("SMSG_READ_ITEM_FAILED's reasons", "[notices]") {
    using F = sn::ReadItemFailure;
    CHECK(sn::readItemFailure(0) == F::Readable);
    CHECK(sn::readItemFailure(1) == F::Translate);
    CHECK(sn::readItemFailure(2) == F::Close);
    CHECK(sn::readItemFailure(255) == F::Close);
}
