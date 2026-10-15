#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "game/packed_time.hpp"

namespace wowee {
namespace network { class Packet; }
namespace game {

/**
 * The calendar the server sends in one packet, unpacked.
 *
 * SMSG_CALENDAR_SEND_CALENDAR is the answer to CMSG_CALENDAR_GET_CALENDAR and
 * carries six lists back to back, each a count followed by its rows. Nothing
 * in it is length-prefixed as a whole and four of the six rows contain a
 * packed guid or a string, so a row read wrong does not fail - it slides
 * everything after it, and the next count is read out of the middle of a
 * string. That is why this is a parser with a test rather than a read done
 * inline: the failure mode is silent and total.
 *
 * Layout, from AzerothCore's WorldSession::HandleCalendarGetCalendar
 * (CalendarHandler.cpp:53) which is the writer this parses:
 *
 *     uint32 numInvites
 *       uint64 eventId, uint64 inviteId, uint8 status, uint8 rank,
 *       uint8 isGuildEvent, packedGuid creator
 *     uint32 numEvents
 *       uint64 eventId, cstring title, uint32 type, packedTime eventTime,
 *       uint32 flags, int32 dungeonId, packedGuid creator
 *     uint32 serverTime, packedTime zoneTime
 *     uint32 numLockouts
 *       uint32 mapId, uint32 difficulty, uint32 secondsLeft, uint64 instanceGuid
 *     uint32 resetRelationTime
 *     uint32 numResets
 *       int32 mapId, int32 period, int32 offset
 *     uint32 numHolidays
 *       uint32 id, region, looping, priority, calendarFilterType,
 *       uint32 date[26], uint32 duration[10], uint32 calendarFlags[10],
 *       cstring textureFilename
 *
 * The holiday row is 49 uint32s and a string; the three array lengths are the
 * server's MAX_HOLIDAY_DATES, MAX_HOLIDAY_DURATIONS and MAX_HOLIDAY_FLAGS
 * (DBCStructure.h:1122). Getting one of those counts wrong misreads every
 * holiday after the first and nothing says so.
 */

/// An invitation waiting on an answer.
struct CalendarInvite {
    uint64_t eventId = 0;
    uint64_t inviteId = 0;
    uint8_t status = 0;        ///< CalendarInviteStatus
    uint8_t rank = 0;          ///< CalendarModerationRank
    bool isGuildEvent = false;
    uint64_t creatorGuid = 0;  ///< the sender's, when the event itself is gone
};

/// An event the player can see, whether or not they have answered it.
struct CalendarEvent {
    uint64_t eventId = 0;
    std::string title;
    uint32_t type = 0;         ///< CalendarEventType
    WowDate eventTime;
    uint32_t eventTimePacked = 0;  ///< kept raw: the interface sorts on it
    uint32_t flags = 0;
    int32_t dungeonId = 0;
    uint64_t creatorGuid = 0;
};

/// A raid the player is saved to, and how long that lasts.
struct CalendarRaidLockout {
    uint32_t mapId = 0;
    uint32_t difficulty = 0;
    uint32_t secondsRemaining = 0;
    uint64_t instanceGuid = 0;
    /// When it runs out, as the calendar places it: the packet's own "now" -
    /// the zone time the calendar carries, or the packed time at the head of
    /// a lockout packet - plus the whole minutes left (0x005c29c0,
    /// 0x005c37d0, 0x005c3880 all add seconds / 60 to that date).
    WowDate expires;
};

/// When a raid's lock resets, and how often.
struct CalendarRaidReset {
    int32_t mapId = 0;
    int32_t periodSeconds = 0;
    int32_t offsetSeconds = 0;
};

/// A world holiday, with the dates it runs on.
struct CalendarHoliday {
    static constexpr int kNumDates = 26;      ///< server MAX_HOLIDAY_DATES
    static constexpr int kNumDurations = 10;  ///< server MAX_HOLIDAY_DURATIONS
    static constexpr int kNumFlags = 10;      ///< server MAX_HOLIDAY_FLAGS

    uint32_t id = 0;
    uint32_t region = 0;
    uint32_t looping = 0;
    uint32_t priority = 0;
    uint32_t calendarFilterType = 0;
    uint32_t dates[kNumDates] = {};
    uint32_t durations[kNumDurations] = {};
    uint32_t calendarFlags[kNumFlags] = {};
    std::string textureFilename;
};

/// Everything the packet carried.
struct CalendarData {
    std::vector<CalendarInvite> invites;
    std::vector<CalendarEvent> events;
    /// The server's own clock, so a client whose clock is wrong still places
    /// events on the right day.
    uint32_t serverTimeUnix = 0;
    WowDate zoneTime;
    std::vector<CalendarRaidLockout> lockouts;
    uint32_t resetRelationTime = 0;
    std::vector<CalendarRaidReset> resets;
    std::vector<CalendarHoliday> holidays;

    /// The client's pending-invite list: one node per invitation still
    /// waiting on an answer, with who sent it (0x005be590 appends,
    /// 0x005becd0 removes the first match). Rebuilt from the invites when the
    /// calendar arrives.
    struct PendingInvite {
        uint64_t eventId = 0;
        uint64_t sender = 0;
    };
    std::vector<PendingInvite> pendingInvites;
    /// SMSG_CALENDAR_SEND_NUM_PENDING's count (0x00c207f4), which the
    /// calendar arriving zeroes.
    uint32_t serverPendingCount = 0;
    /// Whether a whole calendar has arrived this session (0x00c207f2).
    bool received = false;

    /// CalendarGetNumPendingInvites: the larger of the server's count and the
    /// list's, which counts only invitations from a sender who is known and
    /// not on the ignore list (0x005bec70, 0x006b52e0).
    [[nodiscard]] uint32_t pendingInviteCount(
            const std::function<bool(uint64_t)>& isIgnored = {}) const {
        uint32_t listed = 0;
        for (const auto& p : pendingInvites)
            if (p.sender != 0 && !(isIgnored && isIgnored(p.sender))) ++listed;
        return listed > serverPendingCount ? listed : serverPendingCount;
    }
};

/**
 * Read one SMSG_CALENDAR_SEND_CALENDAR.
 *
 * False when the packet runs out mid-row, and `out` is then whatever had been
 * read whole - a truncated calendar rather than a mixture of real values and
 * values read past the end. Every count is checked against what is left before
 * it is trusted: the counts are server-controlled uint32s and a corrupt one
 * would otherwise reserve four billion rows.
 */
bool parseCalendarSendCalendar(network::Packet& packet, CalendarData& out);

/// One person on an event's invite list.
struct CalendarEventInvitee {
    uint64_t guid = 0;
    uint64_t inviteId = 0;
    uint8_t level = 0;
    uint8_t status = 0;        ///< CalendarInviteStatus
    uint8_t rank = 0;          ///< CalendarModerationRank
    bool isGuildMember = false;
    WowDate statusTime;
    std::string note;
};

/**
 * One event in full, as SMSG_CALENDAR_SEND_EVENT sends it.
 *
 * The answer to opening an event. Laid out against CalendarMgr::
 * SendCalendarEvent (CalendarMgr.cpp:627): a header, then a count and that
 * many invitee rows - and every row carries a packed guid, a packed time and
 * a string, so a row read wrong slides the rest exactly as the calendar packet
 * does.
 */
struct CalendarEventDetail {
    uint8_t sendType = 0;
    uint64_t creatorGuid = 0;
    uint64_t eventId = 0;
    std::string title;
    std::string description;
    uint8_t type = 0;
    uint8_t repeatOption = 0;
    uint32_t maxInvites = 0;
    int32_t dungeonId = -1;
    uint32_t flags = 0;
    WowDate eventTime;
    uint32_t eventTimePacked = 0;
    WowDate timeZoneTime;
    uint32_t guildId = 0;
    std::vector<CalendarEventInvitee> invitees;
    /// Invitees whose names are still being asked for, one per lookup the
    /// client counts (0x548), and what to say once the last one is in (0x54c,
    /// 0x005f1a40): 0 CALENDAR_UPDATE_INVITE_LIST, 1 the same with true, 2
    /// CALENDAR_NEW_EVENT, 3 the same for a copy, 4 CALENDAR_OPEN_EVENT. A
    /// fresh event starts at 1 (0x005f29e0).
    std::vector<uint64_t> namesAwaited;
    uint8_t onceNamed = 1;
};

/// Read one SMSG_CALENDAR_SEND_EVENT. False when it runs out mid-row.
bool parseCalendarSendEvent(network::Packet& packet, CalendarEventDetail& out);

/// The calendar's event flags, as 0x005b7fd0 and the handlers test them.
namespace calendar_flag {
constexpr uint32_t kPlayer          = 0x0001;
constexpr uint32_t kSystem          = 0x0004;
constexpr uint32_t kHoliday         = 0x0008;
constexpr uint32_t kInvitesLocked   = 0x0010;
constexpr uint32_t kAnnouncement    = 0x0040;  ///< a guild announcement: no invites
constexpr uint32_t kRaidLockout     = 0x0080;
constexpr uint32_t kRaidReset       = 0x0200;
constexpr uint32_t kGuildEvent      = 0x0400;
}  // namespace calendar_flag

/// CalendarInviteStatus values the handlers act on.
namespace calendar_status {
constexpr uint8_t kInvited     = 0;
constexpr uint8_t kAccepted    = 1;
constexpr uint8_t kConfirmed   = 3;
constexpr uint8_t kSignedUp    = 6;
constexpr uint8_t kNotSignedUp = 7;
constexpr uint8_t kRemoved     = 9;
}  // namespace calendar_status

/// The calendarType string the interface is handed for an event's flags
/// (0x005b7fd0).
const char* calendarTypeForFlags(uint32_t flags);

/// The calendar packets 0x005c3fe0 dispatches, plus the one it reaches by its
/// default case.
enum class CalendarPacket {
    SendCalendar,          // 0x436
    SendEvent,             // 0x437
    FilterGuild,           // 0x438
    ArenaTeam,             // 0x439
    EventInvite,           // 0x43A
    InviteRemoved,         // 0x43B
    EventStatus,           // 0x43C
    LockoutAdded,          // 0x43E
    LockoutRemoved,        // 0x43F
    InviteAlert,           // 0x440
    InviteRemovedAlert,    // 0x441
    InviteStatusAlert,     // 0x442
    EventRemovedAlert,     // 0x443
    EventUpdatedAlert,     // 0x444
    ModeratorStatusAlert,  // 0x445
    SendNumPending,        // 0x448
    InviteNotes,           // 0x460
    InviteNotesAlert,      // 0x461
    LockoutUpdated,        // 0x471
    ClearPendingAction,    // 0x4BB
};

/// One interface event, with its arguments, in the order wow.exe raises it.
struct CalendarSignal {
    std::string event;
    std::vector<std::string> args;
};

/// What a calendar packet changes, and what it says about it.
///
/// `cal` and `open` are the client's two halves: the calendar's event list
/// and the one event slot (0x00c207ec) that SMSG_CALENDAR_SEND_EVENT fills
/// and CalendarNewEvent starts afresh. The handler fills `signals`; the
/// caller raises them as they are.
struct CalendarContext {
    CalendarData& cal;
    CalendarEventDetail& open;
    uint64_t player = 0;
    uint8_t playerLevel = 0;
    std::vector<CalendarSignal> signals;
    /// Ask the server for the pending count (CMSG_CALENDAR_GET_NUM_PENDING),
    /// which two alerts do for an event the calendar has not yet loaded
    /// (0x005bbbb0).
    bool askNumPending = false;
    /// Whether a player is on the ignore list (0x006b52e0). Unset, nobody is.
    std::function<bool(uint64_t)> isIgnored;
    /// Whether a player's name is already known (0x0067d770). Unset, every
    /// name is.
    std::function<bool(uint64_t)> nameKnown;
    /// Names the open event waits on, to be asked for (CMSG_NAME_QUERY).
    std::vector<uint64_t> nameQueries;
    /// Whether `open` is a new event, not yet made (+0x500), whose id is 0:
    /// an invitation to it comes back with event id 0 and is added to its
    /// list (0x005bf030 -> 0x005f32b0).
    bool newEventOpen = false;
};

/// The signal standing for a look for mail, which a removed invitation that
/// leaves a mail behind makes as SMSG_RECEIVED_MAIL does (0x005c0ae0 ->
/// 0x005704d0) - in its place among the events, ahead of
/// CALENDAR_UPDATE_PENDING_INVITES. Not an interface event: the caller looks
/// for mail where it stands.
inline constexpr const char* kCalendarCheckMail = "<check mail>";

/// Read one calendar packet the way 0x005c3fe0 and its handlers read it, and
/// apply it. Every packet but the four alerts that return straight out of the
/// dispatcher - and those whose clear-pending byte says otherwise - ends with
/// CALENDAR_ACTION_PENDING false. False when the packet is cut short, with
/// nothing applied.
bool applyCalendarPacket(CalendarPacket which, network::Packet& packet,
                         CalendarContext& ctx);

/// A name the open event was waiting on has arrived (0x005f1b10 ->
/// 0x005f1a40): once none are left, what the event was holding back is said.
void calendarNameArrived(CalendarEventDetail& open, uint64_t guid,
                         std::vector<CalendarSignal>& out);

/// Whether an invitation sent at `lastInviteMs` (0: none yet) still holds
/// the next back at `nowMs`, both on the client's millisecond clock: two
/// seconds, CALENDAR_ERROR_INVITE_THROTTLED, as CalendarEventInvite
/// (0x005bc360) and CalendarCanSendInvite (0x005b8b30) measure it against
/// 0x00c207f8.
inline bool calendarInviteThrottled(uint32_t lastInviteMs, uint32_t nowMs) {
    return lastInviteMs != 0 && static_cast<int32_t>(nowMs - lastInviteMs) < 2000;
}

/// Whether an event added at `lastAddMs` (0: none yet) still holds the next
/// back at `nowMs`: five seconds, CALENDAR_ERROR_EVENT_THROTTLED, as
/// CalendarAddEvent (0x005bbd60) and CalendarCanAddEvent (0x005b8ba0) measure
/// it against 0x00c207fc.
inline bool calendarAddThrottled(uint32_t lastAddMs, uint32_t nowMs) {
    return lastAddMs != 0 && static_cast<int32_t>(nowMs - lastAddMs) < 5000;
}

/// The invite id an opened event keeps for the player (+0x28): their own
/// row's when it carries a moderation rank, else 0 (0x005f2c00). A new event
/// keeps 1 (0x005f2a20). It is what an invitation sends as its own.
uint64_t calendarOwnInviteId(const CalendarEventDetail& open, uint64_t player);

/// 0x005f1b30: whether the player may invite to the open event - it keeps an
/// invite id for them, or they made it.
inline bool calendarMayInvite(uint64_t ownInviteId, uint64_t creator, uint64_t player) {
    return ownInviteId != 0 || creator == player;
}

/// The new event CalendarNewEvent, CalendarNewGuildAnnouncement and
/// CalendarNewGuildEvent open (0x005f2a20 with 0, 1, 2): the player's, id 0,
/// flags 0x01, 0x40 or 0x400, a hundred invites at most, no dungeon - and on
/// its invite list the player, confirmed and its owner (guild-marked for a
/// guild event), except for an announcement, which lists nobody.
enum class CalendarNewKind : uint8_t { Player = 0, Announcement = 1, GuildEvent = 2 };
CalendarEventDetail calendarNewEvent(CalendarNewKind kind, uint64_t player, uint8_t level);

/// 0x005b7c00: whether a new event may be added on `date` (its day; the time
/// is not looked at). Between 24 November 2004 and 1 January 2031
/// (0x005b7b30), and, with the game clock's date `now` known (0x00d37f98),
/// no earlier than today and no later than the first of the month thirteen
/// on from now's, its year at most 2031 (0x005b7a80). CalendarAddEvent and
/// CalendarContextEventPaste send nothing, and say nothing, when it is not.
bool calendarDateAddable(const WowDate& date, const WowDate* now);

/// 0x0076ccc0 against the game clock: whether `when` is now or past, to the
/// minute. False with the clock not known.
bool calendarTimePassed(const WowDate& when, const WowDate* now);

/// A date moved on by whole minutes, weekday and all - what 0x0076c3f0 does
/// to the date it is handed.
WowDate addMinutes(const WowDate& date, int64_t minutes);

/// An event being built up before it is sent.
///
/// The interface fills this in over several calls - a title here, a date
/// there - and commits it with one, so it has to be held somewhere between
/// them. That is how WoW works too: CalendarNewEvent starts one,
/// CalendarEventSetTitle and friends fill it, CalendarAddEvent sends it.
struct CalendarEventDraft {
    std::string title;
    std::string description;
    uint8_t type = 0;            ///< CalendarEventType
    uint8_t repeatOption = 0;    ///< CalendarRepeatType: 0 never, 1 weekly, 2 fortnightly
    uint32_t maxInvites = 100;
    int32_t dungeonId = -1;      ///< -1 for no dungeon, and signed for that reason
    WowDate eventTime;
    uint32_t flags = 0;
    /// Whether CalendarEventSetDate / CalendarEventSetTime have given the
    /// date and the time: a new event starts with neither (0x005f2a20 sets
    /// both to -1, 0x0076c190), and CalendarAddEvent refuses it without them.
    bool hasDate = false;
    bool hasTime = false;
};

/// CMSG_CALENDAR_ADD_EVENT's body as 0x005f1b90 writes it: title,
/// description, type, repeat, max invites, dungeon, the time twice, the
/// flags, then - unless it is an announcement - the invite list's count and
/// each row's packed guid, status and rank.
void writeCalendarAddEvent(network::Packet& packet, const CalendarEventDraft& draft,
                           uint32_t flags, const std::vector<CalendarEventInvitee>& invitees);

/// What a row on a given day came from. The interface reads a day's rows
/// through one index space - `CalendarGetNumDayEvents` counts them and
/// `CalendarGetDayEvent`, `CalendarGetHolidayInfo` and `CalendarGetRaidInfo`
/// all index into the same list - so the order has to be decided once, here,
/// rather than in each of those.
enum class CalendarEntryKind { Holiday, RaidLockout, Event };

struct CalendarDayEntry {
    CalendarEntryKind kind = CalendarEntryKind::Event;
    /// Into CalendarData::holidays, ::lockouts or ::events, by kind.
    size_t index = 0;
    /// For a raid lockout, the hour and minute it expires. The packet gives
    /// seconds remaining rather than a time, so it is worked out once here
    /// with the rest of the day's placement rather than at each reader.
    int hour = 0;
    int minute = 0;
    /// True on every day of a holiday after the first. FrameXML skips a row
    /// whose sequenceType is "ONGOING" when drawing the day's list, so a
    /// week-long holiday draws its name once rather than seven times.
    bool ongoing = false;
};

/**
 * The rows for one day, holidays first and then events by time.
 *
 * Holidays lead because that is the order WoW draws them in - a world event
 * heads the day and the player's own events follow underneath.
 *
 * A holiday covers `durations[i]` hours from `dates[i]`, and both arrays are
 * walked together: a holiday with several date/duration pairs is several
 * separate runs in the same row, which is how a recurring one is sent.
 */
std::vector<CalendarDayEntry> calendarEntriesForDay(const CalendarData& data,
                                                    int month, int day,
                                                    int year);

}  // namespace game
}  // namespace wowee
