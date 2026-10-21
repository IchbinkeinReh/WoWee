#pragma once

/**
 * server_notices.hpp - the game errors a handful of server packets come down
 * to, and which line each one goes to.
 *
 * Every one of these is a row of the client's game error table (0x00ac8af0,
 * twenty bytes a row, read by 0x005216f0): a GlobalStrings key, and whether
 * the words go to chat as a system line (0), to the yellow UI_INFO_MESSAGE
 * line (1) or to the red UI_ERROR_MESSAGE line (2). The indices are kept next
 * to each row so a reader can find it in the table; the enUS wording is the
 * interface's, for want of one.
 */

#include <cstdint>

namespace wowee {
namespace game {
namespace server_notice {

/// Where 0x005216f0 puts a game error, as the row's second word says.
enum class Line : uint8_t { Chat = 0, Info = 1, Error = 2 };

struct GameError {
    uint16_t index;     ///< row of the table at 0x00ac8af0
    const char* key;    ///< the GlobalStrings name
    const char* enUS;   ///< its enUS words
    Line line;
};

/// MSG_QUEST_PUSH_RESULT (0x006e2e90 case 0x276): guid, uint8 result. Results
/// 0..11 are game errors 0x19b..0x1a6 with the player's name; anything else
/// says nothing. All twelve go to chat.
inline const GameError* questPushResult(uint8_t result) {
    static constexpr GameError kRows[] = {
        {0x19b, "ERR_QUEST_PUSH_SUCCESS_S", "Sharing quest with %s...", Line::Chat},
        {0x19c, "ERR_QUEST_PUSH_INVALID_S", "%s is not eligible for that quest", Line::Chat},
        {0x19d, "ERR_QUEST_PUSH_ACCEPTED_S", "%s has accepted your quest", Line::Chat},
        {0x19e, "ERR_QUEST_PUSH_DECLINED_S", "%s has declined your quest", Line::Chat},
        {0x19f, "ERR_QUEST_PUSH_BUSY_S", "%s is busy", Line::Chat},
        {0x1a0, "ERR_QUEST_PUSH_LOG_FULL_S", "%s's quest log is full", Line::Chat},
        {0x1a1, "ERR_QUEST_PUSH_ONQUEST_S", "%s is already on that quest", Line::Chat},
        {0x1a2, "ERR_QUEST_PUSH_ALREADY_DONE_S", "%s has completed that quest", Line::Chat},
        {0x1a3, "ERR_QUEST_PUSH_NOT_DAILY_S", "That quest cannot be shared today", Line::Chat},
        {0x1a4, "ERR_QUEST_PUSH_TIMER_EXPIRED_S", "Quest sharing timer has expired", Line::Chat},
        {0x1a5, "ERR_QUEST_PUSH_NOT_IN_PARTY_S", "You are not in a party", Line::Chat},
        {0x1a6, "ERR_QUEST_PUSH_DIFFERENT_SERVER_DAILY_S", "%s is not eligible for that quest today",
         Line::Chat},
    };
    return result < sizeof(kRows) / sizeof(kRows[0]) ? &kRows[result] : nullptr;
}

/// MSG_NOTIFY_PARTY_SQUELCH (0x005725c0): uint32, zero for a voice given back.
/// Said to the player whose voice it is.
inline const GameError& partySquelch(uint32_t silenced) {
    static constexpr GameError kUnsilenced{0x241, "ERR_PLAYER_UNSILENCED",
                                           "A group leader has restored your voice privileges.", Line::Chat};
    static constexpr GameError kSilenced{0x240, "ERR_PLAYER_SILENCED",
                                         "A group leader has removed your voice privileges.", Line::Chat};
    return silenced == 0 ? kUnsilenced : kSilenced;
}

/// SMSG_ECHO_PARTY_SQUELCH (0x00572610): guid, uint32 as above. Said to the
/// leader who did it, with the other player's name.
inline const GameError& partySquelchEcho(uint32_t silenced) {
    static constexpr GameError kUnsilenced{0x250, "ERR_PLAYER_UNSILENCED_ECHO",
                                           "You have restored the voice privileges of %s.", Line::Chat};
    static constexpr GameError kSilenced{0x24f, "ERR_PLAYER_SILENCED_ECHO",
                                         "You have removed the voice privileges of %s.", Line::Chat};
    return silenced == 0 ? kUnsilenced : kSilenced;
}

// The opcodes that are one game error and nothing else, each from a handler
// that pushes the index and calls 0x005216f0.
inline constexpr GameError kComsatReconnectAttempt{  // SMSG_COMSAT_RECONNECT_TRY, 0x004fb620
    0x243, "ERR_COMSAT_RECONNECT_ATTEMPT", "Voice Chat service restored!", Line::Chat};
inline constexpr GameError kComsatDisconnect{  // SMSG_COMSAT_DISCONNECT, 0x004fb640
    0x242, "ERR_COMSAT_DISCONNECT", "Connection lost to Voice Chat service.", Line::Chat};
inline constexpr GameError kComsatConnectFail{  // SMSG_COMSAT_CONNECT_FAIL, 0x004fb660
    0x244, "ERR_COMSAT_CONNECT_FAIL", "Cannot connect to voice chat service.", Line::Chat};
inline constexpr GameError kVoiceSessionFull{  // SMSG_VOICESESSION_FULL, 0x004fb680
    0x251, "ERR_VOICESESSION_FULL", "The voice session you are attempting to join is full.", Line::Chat};
inline constexpr GameError kBattlegroundInfoThrottled{  // SMSG_BATTLEGROUND_INFO_THROTTLED, 0x006cbea0
    0x293, "ERR_BATTLEGROUND_INFO_THROTTLED", "You can't do that yet", Line::Chat};
inline constexpr GameError kInvalidPromotionCode{  // SMSG_INVALID_PROMOTION_CODE, 0x00526530
    0x1e5, "ERR_INVALID_PROMOTION_CODE", "Couldn't validate code, please try again.", Line::Error};
inline constexpr GameError kArenaTeamChangeFailedQueued{  // SMSG_ARENA_TEAM_CHANGE_FAILED_QUEUED, 0x006e2e90
    0x295, "ERR_ARENA_TEAM_CHANGE_FAILED_QUEUED", "Can't modify arena team while queued or in a match.",
    Line::Error};
inline constexpr GameError kGmResponseDbError{  // SMSG_GMRESPONSE_DB_ERROR, 0x005ad240
    0x2a1, "ERR_GMRESPONSE_DB_ERROR", "Error retrieving GM response.", Line::Error};

/// SMSG_GM_TICKET_STATUS_UPDATE (0x005ad240 case 0x328): uint32 1..4 through
/// the jump table at 0x005ad5ec.
enum class TicketStatusAction : uint8_t {
    None,           ///< outside 1..4
    Refresh,        ///< 1 and 4: ask for the ticket again (0x005acbf0, CMSG_GMTICKET_GETTICKET)
    Closed,         ///< 2: the ticket is gone - UPDATE_TICKET
    ClosedSurvey,   ///< 3: the ticket is gone and a survey offered - GMSURVEY_DISPLAY
};

inline TicketStatusAction ticketStatusAction(uint32_t status) {
    switch (status) {
        case 1: case 4: return TicketStatusAction::Refresh;
        case 2: return TicketStatusAction::Closed;
        case 3: return TicketStatusAction::ClosedSurvey;
        default: return TicketStatusAction::None;
    }
}

/// SMSG_READ_ITEM_FAILED (0x006d5d90): item guid, uint8 reason - and for
/// reason 1 a uint32 of milliseconds.
enum class ReadItemFailure : uint8_t {
    Readable,    ///< 0: shown as SMSG_READ_ITEM_OK shows it (0x00589e90 with 1)
    Translate,   ///< 1: shown untranslated, and ITEM_TEXT_TRANSLATION with the delay in seconds
    Close,       ///< anything else: ITEM_TEXT_CLOSED, if it is the item being read
};

inline ReadItemFailure readItemFailure(uint8_t reason) {
    if (reason == 0) return ReadItemFailure::Readable;
    if (reason == 1) return ReadItemFailure::Translate;
    return ReadItemFailure::Close;
}

} // namespace server_notice
} // namespace game
} // namespace wowee
